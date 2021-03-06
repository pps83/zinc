/**
 * MIT License
 *
 * Copyright (c) 2017 Rokas Kupstys
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <cassert>
#include "DeltaResolver.h"
#include "RollingChecksum.hpp"
#include "Utilities.hpp"


namespace zinc
{

DeltaResolver::DeltaResolver(IFile* file, size_t block_size,
                             const RemoteFileHashList& hashes, size_t thread_count)
    : _hashes(&hashes)
    , _block_size(block_size)
    , Task<DeltaMap>(file, thread_count)
{
    assert(file != nullptr);
    assert(block_size > 0);
    assert(!hashes.empty());
    assert(thread_count > 0);

    queue_tasks();
}

void DeltaResolver::queue_tasks()
{
    // Initialize helper data
    _bytes_done = 0;
#if ZINC_USE_SKA_FLAT_HASH_MAP
    lookup_table.reserve(_hashes->size());
    ska::flat_hash_map<StrongHash, std::set<int64_t>, StrongHashHashFunction> identical_blocks;
    identical_blocks.reserve(_bytes_total / _block_size + 1);
#else
    std::unordered_map<StrongHash, std::set<int64_t>> identical_blocks;
#endif

    _result.map.reserve(_hashes->size());
    for (int64_t block_index = 0; block_index < _hashes->size(); block_index++)
        _result.map.emplace_back(DeltaElement{block_index, block_index * _block_size});

    {
        int64_t block_index = 0;
        for (const auto& h : *_hashes)
        {
            lookup_table[h.weak][h.strong] = block_index;
            identical_blocks[h.strong].insert(block_index);
            block_index++;
        }
    }

    // Remove lists of identical hashes if list has one entry. In this case block is not identical with any other block.
    for (auto& identical_block : identical_blocks)
    {
        if (identical_block.second.size() > 1)
        {
            for (int64_t index : identical_block.second)
            {
                std::set<int64_t> identical_blocks_copy(identical_block.second);
                identical_blocks_copy.erase(index);
                _result.identical_blocks[index] = std::move(identical_blocks_copy);
            }
        }
    }

    // Queue threads
    int64_t thread_chunk_size = 10 * 1024 * 1024;
    auto total_thread_count = _bytes_total / thread_chunk_size + 1;
    for (size_t i = 0; i < total_thread_count; i++)
    {
        auto start = thread_chunk_size * i;
        auto length = std::min<int64_t>(thread_chunk_size, _bytes_total - (thread_chunk_size * i));
        if (length > 0)
            _pool.enqueue(&DeltaResolver::process, this, start, length);
    }
}

void DeltaResolver::process(int64_t start_index, int64_t block_length)
{
    block_length = std::min(_bytes_total - start_index, block_length);                 // Make sure we do not
    // go out of bounds.
#if ZINC_USE_SKA_FLAT_HASH_MAP
    ska::flat_hash_map<int64_t, StrongHash> local_hash_cache;
        local_hash_cache.reserve(_bytes_total / _block_size + 1);
#else
    std::unordered_map<int64_t, StrongHash> local_hash_cache;
#endif

    // `- block_size` at the end compensates for w_start being increased on the first pass when weak checksum is
    // empty. Checksum is always empty on the first pass.
    int64_t w_start_oft = start_index - _block_size;
    if (start_index >= _block_size)
    {
        // `- block_size + 1` adjusts starting pointer so that we start consuming bytes in specified block from the
        // first one. Naturally it must include `block_size - 1` bytes from previous block so that rolling hash is
        // calculated through entire file. Previous block stops after consuming last byte before `start_index`.
        w_start_oft = w_start_oft - _block_size + 1;
    }
    RollingChecksum weak;
    WeakHash last_failed_weak = 0;
    int64_t bytes_consumed = 0;
    bool last_failed = false;
    const auto last_local_hash_check_offset = _bytes_total - _block_size;
    uint8_t prev_block_first_byte = 0;

    for (; block_length > 0;)
    {
        // Progress reporting.
        if (bytes_consumed >= _block_size)
        {
            _bytes_done += bytes_consumed;
            bytes_consumed = 0;
            if (_cancel.load())
                return;
        }

        auto current_block_size = (size_t)std::min<int64_t>(_block_size, block_length);
        IFile::DataPointer block(nullptr, [](const void*){});
        if (weak.isEmpty())
        {
            w_start_oft += current_block_size;
            bytes_consumed += current_block_size;
            block_length -= current_block_size;
            block = _file->read(w_start_oft, current_block_size);
            weak.update(block.get(), current_block_size);
        }
        else
        {
            bytes_consumed += 1;
            block_length -= 1;
            ++w_start_oft;
            block = _file->read(w_start_oft, current_block_size);
            weak.rotate(prev_block_first_byte, *((uint8_t*)block.get() + (current_block_size - 1)));
        }
        prev_block_first_byte = *(uint8_t*)block.get();

        WeakHash weak_digest = weak.digest();
        if (last_failed && last_failed_weak == weak_digest)
        {
            // Corner-case optimization for repeating data patterns. For example if old file contained a huge blob of
            // null bytes and new file contains weak hash collision but not not region with same null bytes then
            // algorithm would continue computing strong hashes shifting one byte at a time and fail to find a match.
            // We cache value of last failed weak hash if it's strong hash lookup fails. If next weak hash is same as
            // the last - entire region is skipped. This greatly improves speed in some cases.
            continue;
        }

        auto it = lookup_table.find(weak_digest);
        if (it != lookup_table.end())
        {
            auto strong = strong_hash(block.get(), current_block_size);
            auto jt = it->second.find(strong);
            if (jt != it->second.end())
            {
                if (last_failed)
                    last_failed = false;

                int64_t this_block_index = jt->second;
                auto block_offset = this_block_index * _block_size;

                // In some cases current block may contain identical data with some later blocks. However these
                // later blocks may already have correct data present. This check avoids moving data between blocks
                // if they are identical already.
                if (w_start_oft != block_offset && block_offset < last_local_hash_check_offset)
                {
                    auto lh_it = local_hash_cache.find(block_offset);
                    bool is_identical;
                    if (lh_it == local_hash_cache.end())
                    {
                        auto this_block = _file->read(block_offset, _block_size);
                        StrongHash local_hash = strong_hash(this_block.get(), static_cast<size_t>(_block_size));
                        local_hash_cache[block_offset] = local_hash;
                        is_identical = local_hash == strong;
                    }
                    else
                        is_identical = lh_it->second == strong;

                    if (is_identical)
                    {
                        // Block at `this_block_index` contains identical data as currently inspected block. No need
                        // to update that block.
                        weak.clear();
                        continue;
                    }
                }

                _result.map[this_block_index].local_offset = w_start_oft;
                weak.clear();
            }
            else
            {
                last_failed = true;
                last_failed_weak = weak_digest;
            }
        }
        else
        {
            last_failed = true;
            last_failed_weak = weak_digest;
        }
    }

    // Ensure all bytes are reported.
    _bytes_done += bytes_consumed;
}

DeltaMap& DeltaResolver::result()
{
    // TODO: Fix this ugly workaround.
    // When task is done resolving deltas it keeps file mapping open. This prevents file patching function opening a new
    // file mapping if task object is not destroyed yet. By closing mapping here we allow user to dispose of task object
    // whenever it is convenient, like when object goes out of scope, after patching the file.
    if (success())
        _mapping.close();
    return _result;
}

}
