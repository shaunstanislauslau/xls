// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/jit/llvm_ir_jit.h"

#include <random>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/strings/substitute.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/ir_evaluator_test.h"
#include "xls/ir/value_helpers.h"
#include "re2/re2.h"

namespace xls {
namespace {

using status_testing::IsOkAndHolds;

INSTANTIATE_TEST_SUITE_P(
    LlvmIrJitTest, IrEvaluatorTest,
    testing::Values(IrEvaluatorTestParam(
        [](Function* function,
           const std::vector<Value>& args) -> xabsl::StatusOr<Value> {
          XLS_ASSIGN_OR_RETURN(auto jit, LlvmIrJit::Create(function));
          return jit->Run(args);
        },
        [](Function* function,
           const absl::flat_hash_map<std::string, Value>& kwargs)
            -> xabsl::StatusOr<Value> {
          XLS_ASSIGN_OR_RETURN(auto jit, LlvmIrJit::Create(function));
          return jit->Run(kwargs);
        })));

// This test verifies that a compiled JIT function can be re-used.
TEST(LlvmIrJitTest, ReuseTest) {
  Package package("my_package");
  std::string ir_text = R"(
  fn get_identity(x: bits[8]) -> bits[8] {
    ret identity.1: bits[8] = identity(x)
  }
  )";
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));

  XLS_ASSERT_OK_AND_ASSIGN(auto jit, LlvmIrJit::Create(function));
  EXPECT_THAT(jit->Run({Value(UBits(2, 8))}), IsOkAndHolds(Value(UBits(2, 8))));
  EXPECT_THAT(jit->Run({Value(UBits(4, 8))}), IsOkAndHolds(Value(UBits(4, 8))));
  EXPECT_THAT(jit->Run({Value(UBits(7, 8))}), IsOkAndHolds(Value(UBits(7, 8))));
}

// Verifies that the QuickCheck mechanism can find counter-examples for a simple
// erroneous function.
//
// Chances of this succeeding erroneously are (1/2)^1000.
TEST(LlvmIrJitTest, QuickCheckBits) {
  Package package("bad_bits_property");
  std::string ir_text = R"(
  fn adjacent_bits(x: bits[2]) -> bits[1] {
    first_bit: bits[1] = bit_slice(x, start=0, width=1)
    second_bit: bits[1] = bit_slice(x, start=1, width=1)
    ret eq_value: bits[1] = eq(first_bit, second_bit)
  }
  )";
  int64 seed = 0;
  int64 num_tests = 1000;
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));
  XLS_ASSERT_OK_AND_ASSIGN(auto quickcheck_info,
                           CreateAndQuickCheck(function, seed, num_tests));
  std::vector<Value> results = quickcheck_info.second;
  // If a counter-example was found, the last result will be 0.
  EXPECT_EQ(results.back(), Value(UBits(0, 1)));
}

// Chances of this succeeding erroneously are (1/256)^1000.
TEST(LlvmIrJitTest, QuickCheckArray) {
  Package package("bad_array_property");
  std::string ir_text = R"(
  fn adjacent_elements(x: bits[8][5]) -> bits[1] {
    index.0: bits[32] = literal(value=0)
    index.1: bits[32] = literal(value=1)
    first_element: bits[8] = array_index(x, index.0)
    second_element: bits[8] = array_index(x, index.1)
    ret eq_value: bits[1] = eq(first_element, second_element)
  }
  )";
  int64 seed = 0;
  int64 num_tests = 1000;
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));
  XLS_ASSERT_OK_AND_ASSIGN(auto quickcheck_info,
                           CreateAndQuickCheck(function, seed, num_tests));
  std::vector<Value> results = quickcheck_info.second;
  EXPECT_EQ(results.back(), Value(UBits(0, 1)));
}

// Chances of this succeeding erroneously are (1/256)^1000.
TEST(LlvmIrJitTest, QuickCheckTuple) {
  Package package("bad_tuple_property");
  std::string ir_text = R"(
  fn adjacent_elements(x: (bits[8], bits[8])) -> bits[1] {
    first_member: bits[8] = tuple_index(x, index=0)
    second_member: bits[8] = tuple_index(x, index=1)
    ret eq_value: bits[1] = eq(first_member, second_member)
  }
  )";
  int64 seed = 0;
  int64 num_tests = 1000;
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));
  XLS_ASSERT_OK_AND_ASSIGN(auto quickcheck_info,
                           CreateAndQuickCheck(function, seed, num_tests));
  std::vector<Value> results = quickcheck_info.second;
  EXPECT_EQ(results.back(), Value(UBits(0, 1)));
}

// If the QuickCheck mechanism can't find a falsifying example, we expect
// the argsets and results vectors to have lengths of 'num_tests'.
TEST(LlvmIrJitTest, NumTests) {
  Package package("always_true");
  std::string ir_text = R"(
  fn ret_true(x: bits[32]) -> bits[1] {
    ret eq_value: bits[1] = eq(x, x)
  }
  )";
  int64 seed = 0;
  int64 num_tests = 5050;
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));
  XLS_ASSERT_OK_AND_ASSIGN(auto quickcheck_info,
                           CreateAndQuickCheck(function, seed, num_tests));

  std::vector<std::vector<Value>> argsets = quickcheck_info.first;
  std::vector<Value> results = quickcheck_info.second;
  EXPECT_EQ(argsets.size(), 5050);
  EXPECT_EQ(results.size(), 5050);
}

// Given a constant seed, we expect the same argsets and results vectors from
// two runs through the QuickCheck mechanism.
//
// We expect this test to fail with a probability of (1/128)^1000.
TEST(LlvmIrJitTest, Seeding) {
  Package package("sometimes_false");
  std::string ir_text = R"(
  fn gt_one(x: bits[8]) -> bits[1] {
    literal.2: bits[8] = literal(value=1)
    ret ugt.3: bits[1] = ugt(x, literal.2)
  }
  )";
  int64 seed = 12345;
  int64 num_tests = 1000;
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));
  XLS_ASSERT_OK_AND_ASSIGN(auto quickcheck_info1,
                           CreateAndQuickCheck(function, seed, num_tests));
  XLS_ASSERT_OK_AND_ASSIGN(auto quickcheck_info2,
                           CreateAndQuickCheck(function, seed, num_tests));

  std::vector<std::vector<Value>> argsets1 = quickcheck_info1.first;
  std::vector<Value> results1 = quickcheck_info1.second;

  std::vector<std::vector<Value>> argsets2 = quickcheck_info2.first;
  std::vector<Value> results2 = quickcheck_info2.second;

  EXPECT_EQ(argsets1, argsets2);
  EXPECT_EQ(results1, results2);
}

// Very basic smoke test for packed types.
TEST(LlvmIrJitTest, PackedSmoke) {
  Package package("my_package");
  std::string ir_text = R"(
  fn get_identity(x: bits[8]) -> bits[8] {
    ret identity.1: bits[8] = identity(x)
  }
  )";
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           Parser::ParseFunction(ir_text, &package));

  XLS_ASSERT_OK_AND_ASSIGN(auto jit, LlvmIrJit::Create(function));
  uint8 input_data[] = {0x5a, 0xa5};
  uint8 output_data;
  PackedBitsView<8> input(input_data, 0);
  PackedBitsView<8> output(&output_data, 0);
  XLS_ASSERT_OK(jit->RunWithPackedViews(input, output));
  EXPECT_EQ(output_data, 0x5a);
}

// Tests PackedBitView<X> input/output handling.
template <int64 kBitWidth>
absl::Status TestPackedBits(std::minstd_rand& bitgen) {
  Package package("my_package");
  std::string ir_template = R"(
  fn get_identity(x: bits[$0], y:bits[$0]) -> bits[$0] {
    ret add.1: bits[$0] = add(x, y)
  }
  )";
  std::string ir_text = absl::Substitute(ir_template, kBitWidth);
  XLS_ASSIGN_OR_RETURN(Function * function,
                       Parser::ParseFunction(ir_text, &package));
  XLS_ASSIGN_OR_RETURN(auto jit, LlvmIrJit::Create(function));
  Value v = RandomValue(package.GetBitsType(kBitWidth), &bitgen);
  Bits a(v.bits());
  v = RandomValue(package.GetBitsType(kBitWidth), &bitgen);
  Bits b(v.bits());
  Bits expected = bits_ops::Add(a, b);

  int64 byte_width = CeilOfRatio(kBitWidth, kCharBit);
  auto output_data = std::make_unique<uint8[]>(byte_width);
  bzero(output_data.get(), byte_width);

  auto a_vector = a.ToBytes();
  std::reverse(a_vector.begin(), a_vector.end());
  auto b_vector = b.ToBytes();
  std::reverse(b_vector.begin(), b_vector.end());
  auto expected_vector = expected.ToBytes();
  std::reverse(expected_vector.begin(), expected_vector.end());
  PackedBitsView<kBitWidth> a_view(a_vector.data(), 0);
  PackedBitsView<kBitWidth> b_view(b_vector.data(), 0);
  PackedBitsView<kBitWidth> output(output_data.get(), 0);
  XLS_RETURN_IF_ERROR(jit->RunWithPackedViews(a_view, b_view, output));
  for (int i = 0; i < CeilOfRatio(kBitWidth, kCharBit); i++) {
    XLS_RET_CHECK(output_data[i] == expected_vector[i])
        << std::hex << ": byte " << i << ": "
        << static_cast<uint64>(output_data[i]) << " vs. "
        << static_cast<uint64>(expected_vector[i]);
  }
  return absl::OkStatus();
}

// Tests sanity of PackedBitsViews in the JIT.
TEST(LlvmIrJitTest, PackedBits) {
  std::minstd_rand bitgen;

  // The usual suspects:
  XLS_ASSERT_OK(TestPackedBits<1>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<2>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<4>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<8>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<16>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<32>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<64>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<128>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<256>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<512>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<1024>(bitgen));

  // Now some weirdos:
  XLS_ASSERT_OK(TestPackedBits<7>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<15>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<44>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<543>(bitgen));
  XLS_ASSERT_OK(TestPackedBits<1000>(bitgen));
}

// Concatenates the contents of several Bits objects into a single one.
// Operates differently than bits_ops::Concat, as input[0] remains the LSbits.
Bits VectorToPackedBits(const std::vector<Bits>& input) {
  BitsRope rope(input[0].bit_count() * input.size());
  for (const Bits& bits : input) {
    for (int i = 0; i < bits.bit_count(); i++) {
      rope.push_back(bits.Get(i));
    }
  }

  return rope.Build();
}

// Utility struct to hold different representations of the same data together.
template <typename ViewT>
struct TestData {
  explicit TestData(const Value& value_in)
      : value(value_in), bytes(FlattenValue(value)), view(bytes.data(), 0) {}
  Value value;
  std::vector<uint8> bytes;
  ViewT view;

  static std::vector<uint8> FlattenValue(const Value& value) {
    BitsRope rope(value.GetFlatBitCount());
    FlattenValue(value, rope);
    std::vector<uint8> bytes = rope.Build().ToBytes();
    std::reverse(bytes.begin(), bytes.end());
    return bytes;
  }

  static void FlattenValue(const Value& value, BitsRope& rope) {
    if (value.IsBits()) {
      rope.push_back(value.bits());
    } else if (value.IsArray()) {
      for (const Value& element : value.elements()) {
        FlattenValue(element, rope);
      }
    } else if (value.IsTuple()) {
      // Tuple elements are declared MSelement to LSelement, so we need to pack
      // them in "reverse" order, so the LSelement is at the LSb.
      for (int i = value.elements().size() - 1; i >= 0; i--) {
        FlattenValue(value.elements()[i], rope);
      }
    }
  }
};

// Tests PackedArrayView input/output from the JIT. Takes in an array, an index,
// and a replacement value, and does an array_update(). We then verify that the
// output array looks like expected.
template <int64 kBitWidth, int64 kNumElements>
absl::Status TestSimpleArray(std::minstd_rand& bitgen) {
  using ArrayT = PackedArrayView<PackedBitsView<kBitWidth>, kNumElements>;

  Package package("my_package");
  std::string ir_template = R"(
  fn array_update(array: bits[$0][$1], idx: bits[$0], new_value: bits[$0]) -> bits[$0][$1] {
    ret array_update.4: bits[$0][$1] = array_update(array, idx, new_value)
  }
  )";
  std::string ir_text = absl::Substitute(ir_template, kBitWidth, kNumElements);
  XLS_ASSIGN_OR_RETURN(Function * function,
                       Parser::ParseFunction(ir_text, &package));
  XLS_ASSIGN_OR_RETURN(auto jit, LlvmIrJit::Create(function));

  std::vector<Bits> bits_vector;
  for (int i = 0; i < kNumElements; i++) {
    Value value = RandomValue(package.GetBitsType(kBitWidth), &bitgen);
    bits_vector.push_back(value.bits());
  }
  TestData<ArrayT> array_data(Value(VectorToPackedBits(bits_vector)));

  int index = absl::Uniform(bitgen, 0, kNumElements);
  TestData<PackedBitsView<kBitWidth>> index_data(
      Value(UBits(index, kBitWidth)));

  Value value = RandomValue(package.GetBitsType(kBitWidth), &bitgen);
  TestData<PackedBitsView<kBitWidth>> replacement_data(value);
  bits_vector[index] = replacement_data.value.bits();

  TestData<ArrayT> expected_data(Value(VectorToPackedBits(bits_vector)));

  TestData<ArrayT> output_data(Value(Bits(kBitWidth * kNumElements)));

  XLS_RETURN_IF_ERROR(jit->RunWithPackedViews(array_data.view, index_data.view,
                                              replacement_data.view,
                                              output_data.view));

  for (int i = 0; i < CeilOfRatio(kBitWidth * kNumElements, kCharBit); i++) {
    XLS_RET_CHECK(output_data.bytes[i] == expected_data.bytes[i])
        << std::hex << ": byte " << i << ": "
        << "0x" << static_cast<int>(output_data.bytes[i]) << " vs. "
        << "0x" << static_cast<int>(expected_data.bytes[i]);
  }
  return absl::OkStatus();
}

TEST(LlvmIrJitTest, PackedArrays) {
  std::minstd_rand bitgen;
  XLS_ASSERT_OK((TestSimpleArray<4, 4>(bitgen)));
  XLS_ASSERT_OK((TestSimpleArray<4, 15>(bitgen)));
  XLS_ASSERT_OK((TestSimpleArray<113, 33>(bitgen)));
}

// Creates a simple function to perform a tuple update.
xabsl::StatusOr<Function*> CreateTupleFunction(Package* p,
                                               TupleType* tuple_type,
                                               int64 replacement_index) {
  FunctionBuilder builder("tuple_update", p);
  BValue input_tuple = builder.Param("input_tuple", tuple_type);
  BValue new_element =
      builder.Param("new_element", tuple_type->element_type(replacement_index));
  std::vector<BValue> elements;
  elements.reserve(tuple_type->size());
  for (int i = 0; i < tuple_type->size(); i++) {
    elements.push_back(builder.TupleIndex(input_tuple, i));
  }
  elements[replacement_index] = new_element;
  BValue result_tuple = builder.Tuple(elements);
  return builder.BuildWithReturnValue(result_tuple);
}

// With some template acrobatics, we could eliminate the need for either
// ReplacementT or kReplacementIndex...but I don't think it'd be worth the
// effort.
template <typename TupleT, typename ReplacementT, int kReplacementIndex>
absl::Status TestTuples(std::minstd_rand& bitgen) {
  Package package("my_package");
  TupleType* tuple_type = TupleT::GetFullType(&package);

  Type* replacement_type = tuple_type->element_type(kReplacementIndex);
  XLS_ASSIGN_OR_RETURN(
      Function * function,
      CreateTupleFunction(&package, tuple_type, kReplacementIndex));
  XLS_ASSIGN_OR_RETURN(auto jit, LlvmIrJit::Create(function));

  Value input_tuple = RandomValue(tuple_type, &bitgen);
  TestData<TupleT> input_tuple_data(input_tuple);
  Value replacement = RandomValue(replacement_type, &bitgen);
  TestData<ReplacementT> replacement_data(replacement);

  XLS_ASSIGN_OR_RETURN(std::vector<Value> elements, input_tuple.GetElements());
  elements[kReplacementIndex] = replacement;
  TestData<TupleT> expected_data(Value::Tuple(elements));

  // Braces used to fight the most vexing parse.
  TestData<TupleT> output_data(Value{Bits(TupleT::kBitCount)});
  XLS_RETURN_IF_ERROR(jit->RunWithPackedViews(
      input_tuple_data.view, replacement_data.view, output_data.view));

  for (int i = 0; i < CeilOfRatio(TupleT::kBitCount, kCharBit); i++) {
    XLS_RET_CHECK(output_data.bytes[i] == expected_data.bytes[i])
        << std::hex << ": byte " << i << ": "
        << "0x" << static_cast<int>(output_data.bytes[i]) << " vs. "
        << "0x" << static_cast<int>(expected_data.bytes[i]);
  }

  return absl::OkStatus();
}

TEST(LlvmIrJitTest, PackedTuples) {
  using PackedFloat32T =
      PackedTupleView<PackedBitsView<1>, PackedBitsView<8>, PackedBitsView<23>>;

  std::minstd_rand bitgen;
  {
    using TupleT = PackedTupleView<PackedBitsView<3>, PackedBitsView<7>>;
    using ReplacementT = PackedBitsView<3>;
    XLS_ASSERT_OK((TestTuples<TupleT, ReplacementT, 0>(bitgen)));
  }

  {
    using TupleT = PackedTupleView<PackedBitsView<3>, PackedBitsView<7>>;
    using ReplacementT = PackedBitsView<7>;
    XLS_ASSERT_OK((TestTuples<TupleT, ReplacementT, 1>(bitgen)));
  }

  {
    using TupleT = PackedTupleView<PackedArrayView<PackedFloat32T, 15>,
                                   PackedFloat32T, PackedFloat32T>;
    using ReplacementT = PackedArrayView<PackedFloat32T, 15>;
    XLS_ASSERT_OK((TestTuples<TupleT, ReplacementT, 0>(bitgen)));
  }

  {
    using TupleT = PackedTupleView<PackedArrayView<PackedFloat32T, 15>,
                                   PackedFloat32T, PackedFloat32T>;
    using ReplacementT = PackedFloat32T;
    XLS_ASSERT_OK((TestTuples<TupleT, ReplacementT, 1>(bitgen)));
  }
}

}  // namespace
}  // namespace xls
