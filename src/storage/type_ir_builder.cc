/*
 * type_ir_builder.cc
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "storage/type_ir_builder.h"
#include <string>
#include <utility>
#include "glog/logging.h"

namespace fesql {
namespace storage {
namespace v1 {

int32_t GetStrField(const int8_t* row, uint32_t field_offset,
                    uint32_t next_str_field_offset, uint32_t str_start_offset,
                    uint32_t addr_space, int8_t** data, uint32_t* size) {
    if (row == NULL || data == NULL || size == NULL) return -1;

    const int8_t* row_with_offset = row + str_start_offset;
    uint32_t str_offset = 0;
    uint32_t next_str_offset = 0;
    switch (addr_space) {
        case 1: {
            str_offset =
                (uint8_t)(*(row_with_offset + field_offset * addr_space));
            if (next_str_field_offset > 0) {
                next_str_offset = (uint8_t)(
                    *(row_with_offset + next_str_field_offset * addr_space));
            }
            break;
        }
        case 2: {
            str_offset = *(row_with_offset + field_offset * addr_space);
            if (next_str_field_offset > 0) {
                next_str_offset =
                    *(row_with_offset + next_str_field_offset * addr_space);
            }
            break;
        }
        case 3: {
            const int8_t* cur_row_with_offset =
                row_with_offset + field_offset * addr_space;
            str_offset = (uint8_t)(*(cur_row_with_offset));
            str_offset =
                (str_offset << 8) + (uint8_t)(*(cur_row_with_offset + 1));
            str_offset =
                (str_offset << 8) + (uint8_t)(*(cur_row_with_offset + 2));
            if (next_str_field_offset > 0) {
                const int8_t* next_row_with_offset =
                    row_with_offset + field_offset * addr_space;
                next_str_offset = (uint8_t)(*(next_row_with_offset));
                next_str_offset = (next_str_offset << 8) +
                                  (uint8_t)(*(next_row_with_offset + 1));
                next_str_offset = (next_str_offset << 8) +
                                  (uint8_t)(*(next_row_with_offset + 2));
            }
            break;
        }
        case 4: {
            str_offset =
                (uint32_t)(*(row_with_offset + field_offset * addr_space));
            if (next_str_field_offset > 0) {
                next_str_offset =
                    *(row_with_offset + next_str_field_offset * addr_space);
            }
            break;
        }
        default: {
            return -2;
        }
    }
    const int8_t* ptr = row + str_offset;
    *data = (int8_t*)(ptr);  // NOLINT
    if (next_str_field_offset <= 0) {
        uint32_t total_length = (uint32_t)(*(row + VERSION_LENGTH));
        *size = total_length - str_offset;
    } else {
        *size = next_str_offset - str_offset;
    }
    return 0;
}

int32_t AppendString(int8_t* buf_ptr, uint32_t buf_size, int8_t* val,
                     uint32_t size, uint32_t str_start_offset,
                     uint32_t str_field_offset, uint32_t str_addr_space,
                     uint32_t str_body_offset) {
    uint32_t str_offset = str_start_offset + str_field_offset * str_addr_space;
    if (str_offset + size > buf_size) {
        LOG(WARNING) << "invalid str size expect " << buf_size << " but "
                     << str_offset + size;
        return -1;
    }
    int8_t* ptr_offset = buf_ptr + str_offset;
    switch (str_addr_space) {
        case 1: {
            *(reinterpret_cast<uint8_t*>(ptr_offset)) =
                (uint8_t)str_body_offset;
            break;
        }

        case 2: {
            *(reinterpret_cast<uint16_t*>(ptr_offset)) =
                (uint16_t)str_body_offset;
            break;
        }

        case 3: {
            *(reinterpret_cast<uint8_t*>(ptr_offset)) =
                str_body_offset & 0x0F00;
            *(reinterpret_cast<uint8_t*>(ptr_offset + 1)) =
                str_body_offset & 0x00F0;
            *(reinterpret_cast<uint8_t*>(ptr_offset + 2)) =
                str_body_offset & 0x000F;
            break;
        }

        default: {
            *(reinterpret_cast<uint32_t*>(ptr_offset)) = str_body_offset;
        }
    }

    if (size != 0) {
        memcpy(reinterpret_cast<char*>(buf_ptr + str_body_offset), val, size);
    }

    return str_body_offset + size;
}

}  // namespace v1

void AddSymbol(::llvm::orc::JITDylib& jd,           // NOLINT
               ::llvm::orc::MangleAndInterner& mi,  // NOLINT
               const std::string& fn_name, void* fn_ptr) {
    ::llvm::StringRef symbol(fn_name);
    ::llvm::JITEvaluatedSymbol jit_symbol(
        ::llvm::pointerToJITTargetAddress(fn_ptr), ::llvm::JITSymbolFlags());
    ::llvm::orc::SymbolMap symbol_map;
    symbol_map.insert(std::make_pair(mi(symbol), jit_symbol));
    auto err = jd.define(::llvm::orc::absoluteSymbols(symbol_map));
    if (err) {
        LOG(WARNING) << "fail to add symbol " << fn_name;
    } else {
        LOG(INFO) << "add fn symbol " << fn_name << " done";
    }
}

void InitCodecSymbol(::llvm::orc::JITDylib& jd,             // NOLINT
                     ::llvm::orc::MangleAndInterner& mi) {  // NOLINT
    // decode
    AddSymbol(jd, mi, "fesql_storage_get_int16_field",
              reinterpret_cast<void*>(&v1::GetInt16Field));
    AddSymbol(jd, mi, "fesql_storage_get_int32_field",
              reinterpret_cast<void*>(&v1::GetInt32Field));
    AddSymbol(jd, mi, "fesql_storage_get_int64_field",
              reinterpret_cast<void*>(&v1::GetInt64Field));
    AddSymbol(jd, mi, "fesql_storage_get_float_field",
              reinterpret_cast<void*>(&v1::GetFloatField));
    AddSymbol(jd, mi, "fesql_storage_get_double_field",
              reinterpret_cast<void*>(&v1::GetDoubleField));
    AddSymbol(jd, mi, "fesql_storage_get_str_addr_space",
              reinterpret_cast<void*>(&v1::GetAddrSpace));
    AddSymbol(jd, mi, "fesql_storage_get_str_field",
              reinterpret_cast<void*>(&v1::GetStrField));
    // encode
    AddSymbol(jd, mi, "fesql_storage_encode_int16_field",
              reinterpret_cast<void*>(&v1::AppendInt16));

    AddSymbol(jd, mi, "fesql_storage_encode_int32_field",
              reinterpret_cast<void*>(&v1::AppendInt32));

    AddSymbol(jd, mi, "fesql_storage_encode_int64_field",
              reinterpret_cast<void*>(&v1::AppendInt64));

    AddSymbol(jd, mi, "fesql_storage_encode_float_field",
              reinterpret_cast<void*>(&v1::AppendFloat));

    AddSymbol(jd, mi, "fesql_storage_encode_double_field",
              reinterpret_cast<void*>(&v1::AppendDouble));

    AddSymbol(jd, mi, "fesql_storage_encode_string_field",
              reinterpret_cast<void*>(&v1::AppendString));
    AddSymbol(jd, mi, "fesql_storage_encode_calc_size",
              reinterpret_cast<void*>(&v1::CalcTotalLength));
}

void InitCodecSymbol(vm::FeSQLJIT* jit_ptr) {
    jit_ptr->AddSymbol("fesql_storage_get_int16_field",
                       reinterpret_cast<void*>(&v1::GetInt16Field));
    jit_ptr->AddSymbol("fesql_storage_get_int32_field",
                       reinterpret_cast<void*>(&v1::GetInt32Field));
    jit_ptr->AddSymbol("fesql_storage_get_int64_field",
                       reinterpret_cast<void*>(&v1::GetInt64Field));
    jit_ptr->AddSymbol("fesql_storage_get_float_field",
                       reinterpret_cast<void*>(&v1::GetFloatField));
    jit_ptr->AddSymbol("fesql_storage_get_double_field",
                       reinterpret_cast<void*>(&v1::GetDoubleField));
    jit_ptr->AddSymbol("fesql_storage_get_str_addr_space",
                       reinterpret_cast<void*>(&v1::GetAddrSpace));
    jit_ptr->AddSymbol("fesql_storage_get_str_field",
                       reinterpret_cast<void*>(&v1::GetStrField));

    jit_ptr->AddSymbol("fesql_storage_encode_int16_field",
                       reinterpret_cast<void*>(&v1::AppendInt16));

    jit_ptr->AddSymbol("fesql_storage_encode_int32_field",
                       reinterpret_cast<void*>(&v1::AppendInt32));

    jit_ptr->AddSymbol("fesql_storage_encode_int64_field",
                       reinterpret_cast<void*>(&v1::AppendInt64));

    jit_ptr->AddSymbol("fesql_storage_encode_float_field",
                       reinterpret_cast<void*>(&v1::AppendFloat));

    jit_ptr->AddSymbol("fesql_storage_encode_double_field",
                       reinterpret_cast<void*>(&v1::AppendDouble));

    jit_ptr->AddSymbol("fesql_storage_encode_string_field",
                       reinterpret_cast<void*>(&v1::AppendString));

    jit_ptr->AddSymbol("fesql_storage_encode_calc_size",
                       reinterpret_cast<void*>(&v1::CalcTotalLength));
}

}  // namespace storage
}  // namespace fesql
