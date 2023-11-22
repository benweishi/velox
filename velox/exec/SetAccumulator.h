/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <folly/container/F14Set.h>
#include "velox/common/memory/HashStringAllocator.h"
#include "velox/exec/AddressableNonNullValueList.h"
#include "velox/exec/Strings.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::aggregate::prestosql {

namespace detail {

/// Maintains a set of unique values. Non-null values are stored in F14FastSet.
/// A separate flag tracks presence of the null value.
template <
    typename T,
    typename Hash = std::hash<T>,
    typename EqualTo = std::equal_to<T>>
struct SetAccumulator {
  std::optional<vector_size_t> nullIndex;

  folly::F14FastMap<
      T,
      int32_t,
      Hash,
      EqualTo,
      AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>>
      uniqueValues;

  SetAccumulator(const TypePtr& /*type*/, HashStringAllocator* allocator)
      : uniqueValues{AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>(
            allocator)} {}

  SetAccumulator(Hash hash, EqualTo equalTo, HashStringAllocator* allocator)
      : uniqueValues{
            0,
            hash,
            equalTo,
            AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>(
                allocator)} {}

  /// Adds value if new. No-op if the value was added before.
  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* /*allocator*/) {
    const auto cnt = uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!nullIndex.has_value()) {
        nullIndex = cnt;
      }
    } else {
      uniqueValues.insert(
          {decoded.valueAt<T>(index), nullIndex.has_value() ? cnt + 1 : cnt});
    }
  }

  /// Adds new values from an array.
  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void addSerialized(
      const FlatVector<StringView>& vector,
      vector_size_t index,
      HashStringAllocator* /*allocator*/) {
    VELOX_CHECK(!vector.isNullAt(index));
    auto serialized = vector.valueAt(index);
    vector_size_t serializedNullIndex;
    memcpy(&serializedNullIndex, serialized.data(), kVectorSizeT);
    if (!nullIndex.has_value() && serializedNullIndex != kNoNullIndex) {
      nullIndex = serializedNullIndex;
    }

    T value;
    auto valueSize = sizeof(T);
    size_t valueIndex;
    auto offset = kVectorSizeT;
    while (offset < serialized.size()) {
      memcpy(&value, serialized.data() + offset, valueSize);
      offset += valueSize;
      memcpy(&valueIndex, serialized.data() + offset, kVectorSizeT);
      offset += kVectorSizeT;
      uniqueValues.insert({value, valueIndex});
    }
  }

  /// Returns number of unique values including null.
  size_t size() const {
    return uniqueValues.size() + (nullIndex.has_value() ? 1 : 0);
  }

  /// Copies the unique values and null into the specified vector starting at
  /// the specified offset.
  vector_size_t extractValues(FlatVector<T>& values, vector_size_t offset) {
    for (auto value : uniqueValues) {
      values.set(offset + value.second, value.first);
    }

    if (nullIndex.has_value()) {
      values.setNull(offset + nullIndex.value(), true);
    }

    return nullIndex.has_value() ? uniqueValues.size() + 1
                                 : uniqueValues.size();
  }

  /// Extracts in result[index] a serialized VARBINARY for the Set Values.
  /// This is used for the spill of this accumulator.
  void extractSerialized(const VectorPtr& result, vector_size_t index) {
    // The serialized value is the nullOffset (kNoNullIndex if no null is
    // present) followed by pairs of each unique value and its respective index.
    size_t valueSize = sizeof(T);
    size_t totalBytes =
        (valueSize + kVectorSizeT) * uniqueValues.size() + kVectorSizeT;

    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalBytes, true);
    auto serializedNullIndex =
        nullIndex.has_value() ? nullIndex.value() : kNoNullIndex;
    memcpy(rawBuffer, &serializedNullIndex, kVectorSizeT);

    size_t offset = kVectorSizeT;
    for (auto value : uniqueValues) {
      memcpy(rawBuffer + offset, &(value.first), valueSize);
      offset += valueSize;
      memcpy(rawBuffer + offset, &(value.second), kVectorSizeT);
      offset += kVectorSizeT;
    }
    flatResult->setNoCopy(index, StringView(rawBuffer, totalBytes));
    return;
  }

  void free(HashStringAllocator& allocator) {
    using UT = decltype(uniqueValues);
    uniqueValues.~UT();
  }

  static const vector_size_t kNoNullIndex = -1;
  static constexpr size_t kVectorSizeT = sizeof(vector_size_t);
};

/// Maintains a set of unique strings.
struct StringViewSetAccumulator {
  /// A set of unique StringViews pointing to storage managed by 'strings'.
  SetAccumulator<StringView> base;

  /// Stores unique non-null non-inline strings.
  Strings strings;

  /// Size (in bytes) of the string values. Used for computing serialized
  /// buffer size for spilling.
  size_t stringSetBytes = 0;

  StringViewSetAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{type, allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    const auto cnt = base.uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!base.nullIndex.has_value()) {
        base.nullIndex = cnt;
      }
    } else {
      auto value = decoded.valueAt<StringView>(index);
      addValue(value, base.nullIndex.has_value() ? cnt + 1 : cnt, allocator);
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void addSerialized(
      const FlatVector<StringView>& vector,
      vector_size_t index,
      HashStringAllocator* allocator) {
    VELOX_CHECK(!vector.isNullAt(index));
    auto serialized = vector.valueAt(index);
    auto serializedBuffer = serialized.data();
    auto serializedSize = serialized.size();

    vector_size_t nullIndex;
    memcpy(&nullIndex, serializedBuffer, base.kVectorSizeT);
    if (!base.nullIndex.has_value() && nullIndex != base.kNoNullIndex) {
      base.nullIndex = nullIndex;
    }

    size_t offset = base.kVectorSizeT;
    vector_size_t length;
    vector_size_t valueIndex;
    while (offset < serializedSize) {
      memcpy(&length, serializedBuffer + offset, base.kVectorSizeT);
      offset += 4;

      StringView value = StringView(serializedBuffer + offset, length);
      offset += length;

      memcpy(&valueIndex, serializedBuffer + offset, base.kVectorSizeT);
      offset += base.kVectorSizeT;

      addValue(value, valueIndex, allocator);
    }
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(
      FlatVector<StringView>& values,
      vector_size_t offset) {
    return base.extractValues(values, offset);
  }

  /// Extracts in result[index] a serialized VARBINARY for the String Values.
  /// This is used for the spill of this accumulator.
  void extractSerialized(const VectorPtr& result, vector_size_t index) {
    // nullIndex (or kNoNullIndex) is serialized followed by triples of
    // (valueSize, String value, valueIndex) of the unique values.
    int32_t totalBytes = base.kVectorSizeT + stringSetBytes +
        2 * base.kVectorSizeT * base.uniqueValues.size();

    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalBytes, true);

    auto nullIndex =
        base.nullIndex.has_value() ? base.nullIndex.value() : base.kNoNullIndex;
    memcpy(rawBuffer, &nullIndex, base.kVectorSizeT);

    size_t offset = base.kVectorSizeT;
    vector_size_t valueSize;
    for (const auto& value : base.uniqueValues) {
      valueSize = value.first.size();
      memcpy(rawBuffer + offset, &valueSize, base.kVectorSizeT);
      offset += 4;
      memcpy(rawBuffer + offset, value.first.data(), valueSize);
      offset += valueSize;
      memcpy(rawBuffer + offset, &value.second, base.kVectorSizeT);
      offset += base.kVectorSizeT;
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, totalBytes));
    return;
  }

  void free(HashStringAllocator& allocator) {
    strings.free(allocator);
    using Base = decltype(base);
    base.~Base();
  }

 private:
  void addValue(
      const StringView& value,
      vector_size_t valueIndex,
      HashStringAllocator* allocator) {
    if (base.uniqueValues.contains(value)) {
      return;
    }
    StringView insertValue = value;
    if (!insertValue.isInline()) {
      insertValue = strings.append(value, *allocator);
    }
    base.uniqueValues.insert({insertValue, valueIndex});
    stringSetBytes += insertValue.size();
  }
};

/// Maintains a set of unique arrays, maps or structs.
struct ComplexTypeSetAccumulator {
  /// A set of pointers to values stored in AddressableNonNullValueList.
  SetAccumulator<
      HashStringAllocator::Position,
      AddressableNonNullValueList::Hash,
      AddressableNonNullValueList::EqualTo>
      base;

  /// Stores unique non-null values.
  AddressableNonNullValueList values;

  /// Tracks allocated bytes for sizing during serialization for spill.
  size_t allValuesSize = 0;

  std::unordered_map<
      HashStringAllocator::Position,
      size_t,
      AddressableNonNullValueList::Hash,
      AddressableNonNullValueList::HashEqualTo>
      valuesLengths;

  ComplexTypeSetAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{
            AddressableNonNullValueList::Hash{},
            AddressableNonNullValueList::EqualTo{type},
            allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    const auto cnt = base.uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!base.nullIndex.has_value()) {
        base.nullIndex = cnt;
      }
    } else {
      auto startSize = allocator->cumulativeBytes();
      auto position = values.append(decoded, index, allocator);

      if (!base.uniqueValues
               .insert({position, base.nullIndex.has_value() ? cnt + 1 : cnt})
               .second) {
        values.removeLast(position);
      } else {
        auto valueSize = allocator->cumulativeBytes() - startSize;
        allValuesSize += valueSize;
        valuesLengths.insert({position, valueSize});
      }
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void addSerialized(
      const FlatVector<StringView>& vector,
      vector_size_t index,
      HashStringAllocator* allocator) {
    VELOX_CHECK(!vector.isNullAt(index));
    auto serialized = vector.valueAt(index);
    vector_size_t nullIndex;
    memcpy(&nullIndex, serialized.data(), base.kVectorSizeT);
    if (!base.nullIndex.has_value() && nullIndex != base.kNoNullIndex) {
      base.nullIndex = nullIndex;
    }

    auto offset = base.kVectorSizeT;
    size_t length;
    size_t valueIndex;
    while (offset < serialized.size()) {
      memcpy(&length, serialized.data() + offset, 4);
      offset += 4;

      StringView value = StringView(serialized.data() + offset, length);
      offset += length;

      memcpy(&valueIndex, serialized.data() + offset, base.kVectorSizeT);

      auto startSize = allocator->cumulativeBytes();
      auto position = values.appendSerialized(value, allocator);

      if (!base.uniqueValues.insert({position, valueIndex}).second) {
        values.removeLast(position);
      } else {
        auto valueSize = allocator->cumulativeBytes() - startSize;
        allValuesSize += valueSize;
        valuesLengths.insert({position, valueSize});
      }
    }
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(BaseVector& values, vector_size_t offset) {
    for (const auto& position : base.uniqueValues) {
      AddressableNonNullValueList::read(
          position.first, values, offset + position.second);
    }

    if (base.nullIndex.has_value()) {
      values.setNull(offset + base.nullIndex.value(), true);
    }

    return base.uniqueValues.size() + (base.nullIndex.has_value() ? 1 : 0);
  }

  /// Extracts in result[index] a serialized VARBINARY for the String Values.
  /// This is used for the spill of this accumulator.
  void extractSerialized(const VectorPtr& result, vector_size_t index) {
    // nullIndex is serialized followed by triples of (value size,
    // ComplexType value, value index) of all unique values.
    size_t totalBytes =
        base.kVectorSizeT + (4 + base.kVectorSizeT) * size() + allValuesSize;

    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalBytes, true);

    vector_size_t nullIndex =
        base.nullIndex.has_value() ? base.nullIndex.value() : base.kNoNullIndex;
    memcpy(rawBuffer, &nullIndex, base.kVectorSizeT);

    size_t offset = base.kVectorSizeT;
    for (const auto& value : base.uniqueValues) {
      VELOX_CHECK(valuesLengths.count(value.first) != 0);
      auto length = valuesLengths[value.first];
      memcpy(rawBuffer + offset, &length, 4);
      offset += 4;
      AddressableNonNullValueList::copy(
          value.first, rawBuffer + offset, length);
      offset += length;
      memcpy(rawBuffer + offset, &value.second, base.kVectorSizeT);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, totalBytes));
    return;
  }

  void free(HashStringAllocator& allocator) {
    values.free(allocator);
    using Base = decltype(base);
    base.~Base();
  }
};

template <typename T>
struct SetAccumulatorTypeTraits {
  using AccumulatorType = SetAccumulator<T>;
};

template <>
struct SetAccumulatorTypeTraits<StringView> {
  using AccumulatorType = StringViewSetAccumulator;
};

template <>
struct SetAccumulatorTypeTraits<ComplexType> {
  using AccumulatorType = ComplexTypeSetAccumulator;
};
} // namespace detail

template <typename T>
using SetAccumulator =
    typename detail::SetAccumulatorTypeTraits<T>::AccumulatorType;

} // namespace facebook::velox::aggregate::prestosql
