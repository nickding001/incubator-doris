// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/rowset/segment_v2/bloom_filter_index_reader.h"
#include "olap/rowset/segment_v2/bloom_filter_index_writer.h"
#include "olap/rowset/segment_v2/bloom_filter.h"
#include "olap/key_coder.h"

#include <gtest/gtest.h>

#include "common/logging.h"
#include "env/env.h"
#include "olap/olap_common.h"
#include "olap/types.h"
#include "olap/column_block.h"
#include "util/file_utils.h"
#include "runtime/mem_tracker.h"
#include "runtime/mem_pool.h"

namespace doris {
namespace segment_v2 {

class BloomFilterIndexReaderWriterTest : public testing::Test {
   public:
    BloomFilterIndexReaderWriterTest() { }
    virtual ~BloomFilterIndexReaderWriterTest() {
    }
};

const std::string dname = "./ut_dir/bloom_filter_index_reader_writer_test";

template<FieldType type>
void write_bloom_filter_index_file(const std::string& file_name, const void* values,
                      size_t value_count, size_t null_count,
                      BloomFilterIndexPB* bloom_filter_index_meta) {
    const TypeInfo* type_info = get_type_info(type);
    using CppType = typename CppTypeTraits<type>::CppType;
    FileUtils::create_dir(dname);
    std::string fname = dname + "/" + file_name;
    {
        std::unique_ptr<WritableFile> wfile;
        auto st = Env::Default()->new_writable_file(fname, &wfile);
        ASSERT_TRUE(st.ok());
        std::unique_ptr<BloomFilterIndexWriter> bloom_filter_index_writer;
        BloomFilterOptions bf_options;
        BloomFilterIndexWriter::create(bf_options, type_info, &bloom_filter_index_writer);
        const CppType* vals = (const CppType*)values;
        for (int i = 0; i < value_count;) {
            size_t num = std::min(1024, (int)value_count - i);
            bloom_filter_index_writer->add_values(vals + i, num);
            if (i == 2048) {
                // second page
                bloom_filter_index_writer->add_nulls(null_count);
            }
            st = bloom_filter_index_writer->flush();
            ASSERT_TRUE(st.ok());
            i += 1024;
        }
        st = bloom_filter_index_writer->finish(wfile.get(), bloom_filter_index_meta);
        ASSERT_TRUE(st.ok()) << "writer finish status:" << st.to_string();
        wfile.reset();
    }
}

void get_bloom_filter_reader_iter(const std::string& file_name, const BloomFilterIndexPB& bloom_filter_index_meta,
                            std::unique_ptr<RandomAccessFile>* rfile,
                            BloomFilterIndexReader** reader,
                            std::unique_ptr<BloomFilterIndexIterator>* iter) {
    std::string fname = dname + "/" + file_name;
    auto st = Env::Default()->new_random_access_file(fname, rfile);
    ASSERT_TRUE(st.ok());

    *reader = new BloomFilterIndexReader(rfile->get(), bloom_filter_index_meta);
    st = (*reader)->load();
    ASSERT_TRUE(st.ok());

    st = (*reader)->new_iterator(iter);
    ASSERT_TRUE(st.ok());
}

template <FieldType Type>
void test_bloom_filter_index_reader_writer_template(const std::string file_name,
        typename TypeTraits<Type>::CppType* val, size_t num, size_t null_num,
        typename TypeTraits<Type>::CppType* not_exist_value,
        bool is_slice_type = false) {
     typedef typename TypeTraits<Type>::CppType CppType;
    BloomFilterIndexPB bloom_filter_index_meta;
    write_bloom_filter_index_file<Type>(file_name, val, num, null_num,
                                          &bloom_filter_index_meta);
    {
        std::unique_ptr<RandomAccessFile> rfile;
        BloomFilterIndexReader* reader = nullptr;
        std::unique_ptr<BloomFilterIndexIterator> iter;
        get_bloom_filter_reader_iter(file_name, bloom_filter_index_meta,
                                                    &rfile, &reader, &iter);

        // page 0
        std::unique_ptr<BloomFilter> bf;
        auto st = iter->read_bloom_filter(0, &bf);
        ASSERT_TRUE(st.ok());
        for (int i = 0; i < 1024; ++i) {
            if (is_slice_type) {
                Slice* value = (Slice*)(val + i);
                ASSERT_TRUE(bf->test_bytes(value->data, value->size));
            } else {
                ASSERT_TRUE(bf->test_bytes((char*)&val[i], sizeof(CppType)));
            }
        }

        // page 1
        st = iter->read_bloom_filter(1, &bf);
        ASSERT_TRUE(st.ok());
        for (int i = 1024; i < 2048; ++i) {
            if (is_slice_type) {
                Slice* value = (Slice*)(val + i);
                ASSERT_TRUE(bf->test_bytes(value->data, value->size));
            } else {
                ASSERT_TRUE(bf->test_bytes((char*)&val[i], sizeof(CppType)));
            }
        }

        // page 2
        st = iter->read_bloom_filter(2, &bf);
        ASSERT_TRUE(st.ok());
        for (int i = 2048; i < 3071; ++i) {
            if (is_slice_type) {
                Slice* value = (Slice*)(val + i);
                ASSERT_TRUE(bf->test_bytes(value->data, value->size));
            } else {
                ASSERT_TRUE(bf->test_bytes((char*)&val[i], sizeof(CppType)));
            }
        }
        // test nullptr
        ASSERT_TRUE(bf->test_bytes(nullptr, 1));

        delete reader;
    }
}

TEST_F(BloomFilterIndexReaderWriterTest, test_int) {
    size_t num = 1024 * 3 - 1;
    int* val = new int[num];
    for (int i = 0; i < num; ++i) {
        // there will be 3 bloom filter pages
        val[i] = 10000 + i + 1;
    }

    std::string file_name = "bloom_filter_int";
    int not_exist_value = 18888;
    test_bloom_filter_index_reader_writer_template<OLAP_FIELD_TYPE_INT>(file_name, val, num, 1, &not_exist_value);
    delete[] val;
}

TEST_F(BloomFilterIndexReaderWriterTest, test_bigint) {
    size_t num = 1024 * 3 - 1;
    int64_t* val = new int64_t[num];
    for (int i = 0; i < num; ++i) {
        // there will be 3 bloom filter pages
        val[i] = 100000000 + i + 1;
    }

    std::string file_name = "bloom_filter_bigint";
    int64_t not_exist_value = 18888;
    test_bloom_filter_index_reader_writer_template<OLAP_FIELD_TYPE_BIGINT>(file_name, val, num, 1, &not_exist_value);
    delete[] val;
}

TEST_F(BloomFilterIndexReaderWriterTest, test_varchar_type) {
    size_t num = 1024 * 3 - 1;
    std::string* val = new std::string[num];
    for (int i = 0; i < num; ++i) {
        // there will be 3 bloom filter pages
        val[i] = "prefix_" + std::to_string(i);
    }
    Slice* slices = new Slice[num];
    for (int i = 0; i < num; ++i) {
        // there will be 3 bloom filter pages
        slices[i] = Slice(val[i].c_str(), val[i].size());
    }
    std::string file_name = "bloom_filter_varchar";
    Slice not_exist_value("value_not_exist");
    test_bloom_filter_index_reader_writer_template<OLAP_FIELD_TYPE_VARCHAR>(file_name, slices, num, 1, &not_exist_value, true);
    delete[] val;
    delete[] slices;
}

TEST_F(BloomFilterIndexReaderWriterTest, test_char) {
    size_t num = 1024 * 3 - 1;
    std::string* val = new std::string[num];
    for (int i = 0; i < num; ++i) {
        // there will be 3 bloom filter pages
        val[i] = "prefix_" + std::to_string(10000 + i);
    }
    Slice* slices = new Slice[num];
    for (int i = 0; i < num; ++i) {
        // there will be 3 bloom filter pages
        slices[i] = Slice(val[i].c_str(), val[i].size());
    }
    std::string file_name = "bloom_filter_char";
    Slice not_exist_value("char_value_not_exist");
    test_bloom_filter_index_reader_writer_template<OLAP_FIELD_TYPE_CHAR>(file_name, slices, num, 1, &not_exist_value, true);
    delete[] val;
    delete[] slices;
}

}
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}