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
#include <zinc/zinc.h>
#include <cstring>
#include <thread>


using namespace std::placeholders;
using namespace zinc;


ByteArray string_to_array(const char* string)
{
    auto len = strlen(string);
    ByteArray result(len);
    memcpy(&result.front(), string, len);
    return result;
}

ByteArray get_data(int64_t block_index, size_t block_size, void* user_data)
{
    ByteArray& source = *(ByteArray*)user_data;
    ByteArray result(std::min(block_size, source.size() - (block_index * block_size)));
    memcpy(&result.front(), &source[block_index * block_size], result.size());
    return result;
}

bool data_sync_test(const char* remote, const char* local, size_t block_size)
{
    ByteArray data_remote = string_to_array(remote);
    ByteArray data_local = string_to_array(local);
    ByteArray data_local_copy = data_local;

    // Ensure local data has enough bytes for remote data
    auto local_file_size = std::max(data_local.size(), data_remote.size());
    if (auto remainder = local_file_size % block_size)
        local_file_size += block_size - remainder;
    data_local.resize(local_file_size, 0);

    auto checksums = get_block_checksums(&data_remote.front(), data_remote.size(), block_size,
                                         std::thread::hardware_concurrency())->wait()->result();
    auto delta = get_differences_delta(&data_local.front(), data_local.size(), block_size, checksums, std::thread::hardware_concurrency())->wait()->result();
    patch_file(&data_local.front(), data_local.size(), block_size, delta, std::bind(get_data, _1, _2, &data_remote));

    // Ensure local data does not have more bytes than remote data
    if (data_local.size() > data_remote.size())
        data_local.resize(data_remote.size());

    if (data_local != data_remote)
    {
        data_local_copy.push_back(0);
        data_remote.push_back(0);
        data_local.push_back(0);
        fprintf(stderr, "Local  data: %s\n", &data_local_copy.front());
        fprintf(stderr, "Remote data: %s\n", &data_remote.front());
        fprintf(stderr, "Result data: %s\n", &data_local.front());
        fprintf(stderr, "Block  size: %d\n", (int)block_size);
        fprintf(stderr, "------------------------------------------\n");
        return false;
    }
    fprintf(stderr, "------------------------------------------\n");
    return true;
}
