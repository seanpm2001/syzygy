// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/minidump/minidump.h"

#include <stdint.h>
#include <set>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/utf_string_conversions.h"
#include "gtest/gtest.h"
#include "syzygy/core/unittest_util.h"
#include "syzygy/minidump/unittest_util.h"

namespace minidump {

class MinidumpTest : public testing::Test {
 public:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    dump_file_ = temp_dir_.path().Append(L"minidump.dmp");
  }

  const base::FilePath& dump_file() const { return dump_file_; }

 private:
  base::FilePath dump_file_;
  base::ScopedTempDir temp_dir_;
};

TEST_F(MinidumpTest, OpenSuccedsForValidFile) {
  Minidump minidump;

  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad32Dump()));
  ASSERT_LE(1U, minidump.directory().size());
}

TEST_F(MinidumpTest, OpenFailsForInvalidFile) {
  Minidump minidump;

  // Try opening a non-existing file.
  ASSERT_FALSE(minidump.Open(dump_file()));

  // Create an empty file, opening it should fail.
  {
    base::ScopedFILE tmp(base::OpenFile(dump_file(), "wb"));
  }
  ASSERT_FALSE(minidump.Open(dump_file()));

  // Create a file with a header, but an invalid signature.
  {
    base::ScopedFILE tmp(base::OpenFile(dump_file(), "wb"));

    MINIDUMP_HEADER hdr = {0};
    ASSERT_EQ(sizeof(hdr), fwrite(&hdr, sizeof(char), sizeof(hdr), tmp.get()));
  }
  ASSERT_FALSE(minidump.Open(dump_file()));

  // Create a file with a valid signature, but a zero-length directory.
  {
    base::ScopedFILE tmp(base::OpenFile(dump_file(), "wb"));

    MINIDUMP_HEADER hdr = {0};
    hdr.Signature = MINIDUMP_SIGNATURE;
    ASSERT_EQ(sizeof(hdr), fwrite(&hdr, sizeof(char), sizeof(hdr), tmp.get()));
  }
  ASSERT_FALSE(minidump.Open(dump_file()));

  // Create a file with a valid header, but a missing directory.
  {
    base::ScopedFILE tmp(base::OpenFile(dump_file(), "wb"));

    MINIDUMP_HEADER hdr = {0};
    hdr.Signature = MINIDUMP_SIGNATURE;
    hdr.NumberOfStreams = 10;
    hdr.StreamDirectoryRva = sizeof(hdr);
    ASSERT_EQ(sizeof(hdr), fwrite(&hdr, sizeof(char), sizeof(hdr), tmp.get()));
  }
  ASSERT_FALSE(minidump.Open(dump_file()));
}

TEST_F(MinidumpTest, StreamTest) {
  // Create a file with some data to test the streams.
  {
    base::ScopedFILE tmp(base::OpenFile(dump_file(), "wb"));

    MINIDUMP_HEADER hdr = {0};
    hdr.Signature = MINIDUMP_SIGNATURE;
    hdr.NumberOfStreams = 1;
    hdr.StreamDirectoryRva = sizeof(hdr);
    ASSERT_EQ(sizeof(hdr), fwrite(&hdr, sizeof(char), sizeof(hdr), tmp.get()));

    for (uint32_t i = 0; i < 100; ++i)
      ASSERT_EQ(sizeof(i), fwrite(&i, sizeof(char), sizeof(i), tmp.get()));
  }

  Minidump minidump;
  ASSERT_TRUE(minidump.Open(dump_file()));

  // Make a short, arbitrary location.
  MINIDUMP_LOCATION_DESCRIPTOR loc = { 7, sizeof(MINIDUMP_HEADER) };
  Minidump::Stream test = minidump.GetStreamFor(loc);

  EXPECT_EQ(7U, test.GetRemainingBytes());

  // Read the first integer.
  const uint32_t kSentinel = 0xCAFEBABE;
  uint32_t tmp = kSentinel;
  ASSERT_TRUE(test.ReadAndAdvanceElement(&tmp));
  EXPECT_EQ(0U, tmp);
  EXPECT_EQ(3U, test.GetRemainingBytes());

  // Reading another integer should fail, as the stream doesn't cover it.
  tmp = kSentinel;
  ASSERT_FALSE(test.ReadAndAdvanceElement(&tmp));
  // The failing read must not modify the input.
  EXPECT_EQ(kSentinel, tmp);

  // Try the same thing with byte reads.
  uint8_t bytes[10] = {};
  ASSERT_FALSE(test.ReadAndAdvanceBytes(4, &bytes));

  // A three-byte read should succeed.
  ASSERT_TRUE(test.ReadAndAdvanceBytes(3, &bytes));
  EXPECT_EQ(0U, test.GetRemainingBytes());

  // Little-endian byte order assumed.
  EXPECT_EQ(1U, bytes[0]);
  EXPECT_EQ(0U, bytes[1]);
  EXPECT_EQ(0U, bytes[2]);

  // No moar data.
  EXPECT_FALSE(test.ReadAndAdvanceBytes(1, &bytes));

  // Reset the stream to test reading via a string.
  test = minidump.GetStreamFor(loc);
  std::string data;
  ASSERT_TRUE(test.ReadAndAdvanceBytes(1, &data));
  EXPECT_EQ(6U, test.GetRemainingBytes());
  EXPECT_EQ(1U, data.size());
  EXPECT_EQ(0, data[0]);
}

TEST_F(MinidumpTest, FindNextStream) {
  Minidump minidump;

  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad32Dump()));

  Minidump::Stream sys_info =
      minidump.FindNextStream(nullptr, SystemInfoStream);
  ASSERT_TRUE(sys_info.IsValid());

  MINIDUMP_SYSTEM_INFO info = {};
  EXPECT_TRUE(sys_info.ReadAndAdvanceElement(&info));

  Minidump::Stream invalid =
      minidump.FindNextStream(&sys_info, SystemInfoStream);
  EXPECT_FALSE(invalid.IsValid());
}

TEST_F(MinidumpTest, ReadThreadInfo) {
  Minidump minidump;

  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad32Dump()));

  Minidump::Stream thread_list =
      minidump.FindNextStream(nullptr, ThreadListStream);
  ASSERT_TRUE(thread_list.IsValid());

  ULONG32 num_threads = 0;
  ASSERT_TRUE(thread_list.ReadAndAdvanceElement(&num_threads));

  for (size_t i = 0; i < num_threads; ++i) {
    MINIDUMP_THREAD thread = {};
    ASSERT_TRUE(thread_list.ReadAndAdvanceElement(&thread));

    Minidump::Stream thread_memory = minidump.GetStreamFor(thread.Stack.Memory);
    EXPECT_TRUE(thread_memory.IsValid());

    Minidump::Stream thread_context =
        minidump.GetStreamFor(thread.ThreadContext);
    EXPECT_TRUE(thread_context.IsValid());

    CONTEXT ctx = {};
    EXPECT_TRUE(thread_context.ReadAndAdvanceElement(&ctx));
  }
}

TEST_F(MinidumpTest, ReadAndAdvanceString) {
  wchar_t kSomeString[] = L"some string";

  // Create a minimal file to test reading a string.
  {
    base::ScopedFILE tmp(base::OpenFile(dump_file(), "wb"));

    // Valid header.
    MINIDUMP_HEADER hdr = {0};
    hdr.Signature = MINIDUMP_SIGNATURE;
    hdr.NumberOfStreams = 1;
    hdr.StreamDirectoryRva = sizeof(hdr);
    ASSERT_EQ(sizeof(hdr), fwrite(&hdr, sizeof(char), sizeof(hdr), tmp.get()));

    // Dummy directory.
    MINIDUMP_DIRECTORY directory = {0};
    ASSERT_EQ(sizeof(directory),
              fwrite(&directory, sizeof(char), sizeof(directory), tmp.get()));

    // A string. Note that although a null terminating character is written, it
    // is not counted in the size written to the file.
    ULONG32 size_bytes = sizeof(kSomeString) - sizeof(wchar_t);
    ASSERT_EQ(sizeof(ULONG32),
              fwrite(&size_bytes, sizeof(char), sizeof(ULONG32), tmp.get()));
    ASSERT_EQ(sizeof(kSomeString), fwrite(&kSomeString, sizeof(char),
                                          sizeof(kSomeString), tmp.get()));
  }

  Minidump minidump;
  ASSERT_TRUE(minidump.Open(dump_file()));

  MINIDUMP_LOCATION_DESCRIPTOR loc = {
      static_cast<ULONG32>(-1),
      sizeof(MINIDUMP_HEADER) + sizeof(MINIDUMP_DIRECTORY)};
  Minidump::Stream test = minidump.GetStreamFor(loc);
  std::wstring recovered;
  ASSERT_TRUE(test.ReadAndAdvanceString(&recovered));
  ASSERT_EQ(kSomeString, recovered);
}

TEST_F(MinidumpTest, GetMemoryList) {
  Minidump minidump;
  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad32Dump()));

  auto memory = minidump.GetMemoryList();
  EXPECT_TRUE(memory.IsValid());
  EXPECT_NE(0U, memory.header().NumberOfMemoryRanges);

  // TODO(siggi): what to do here?
  size_t memory_count = 0;
  size_t memory_size = 0;
  for (const auto& element : memory) {
    ++memory_count;
    memory_size += element.Memory.DataSize;
  }

  ASSERT_EQ(memory.header().NumberOfMemoryRanges, memory_count);
  ASSERT_LT(0u, memory_size);
}

TEST_F(MinidumpTest, GetModuleList) {
  Minidump minidump;
  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad32Dump()));

  auto modules = minidump.GetModuleList();
  EXPECT_TRUE(modules.IsValid());
  EXPECT_NE(0U, modules.header().NumberOfModules);

  // TODO(siggi): what to do here?
  size_t module_count = 0;
  size_t module_size = 0;
  for (const auto& element : modules) {
    ++module_count;
    module_size += element.SizeOfImage;
  }

  ASSERT_EQ(modules.header().NumberOfModules, module_count);
  ASSERT_LT(0u, module_size);
}

TEST_F(MinidumpTest, GetThreadList) {
  Minidump minidump;
  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad32Dump()));

  auto threads = minidump.GetThreadList();
  EXPECT_TRUE(threads.IsValid());
  EXPECT_NE(0U, threads.header().NumberOfThreads);

  std::set<uint32_t> thread_id_set;
  for (const auto& element : threads) {
    ASSERT_TRUE(thread_id_set.insert(element.ThreadId).second);
  }

  ASSERT_LT(0u, thread_id_set.size());
}

#if 0
// TODO(siggi): This is apparently itanium-specific :/.
TEST_F(MinidumpTest, GetThreadExList) {
  Minidump minidump;
  ASSERT_TRUE(minidump.Open(testing::TestMinidumps::GetNotepad64Dump()));

  auto threads = minidump.GetThreadExList();
  EXPECT_TRUE(threads.IsValid());
  EXPECT_NE(0U, threads.header().NumberOfThreads);

  std::set<uint32_t> thread_id_set;
  for (const auto& element : threads) {
    ASSERT_TRUE(thread_id_set.insert(element.ThreadId).second);
  }

  ASSERT_LT(0u, thread_id_set.size());
}
#endif

}  // namespace minidump
