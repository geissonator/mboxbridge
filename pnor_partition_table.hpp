/*
 * Mailbox Daemon Implementation
 *
 * Copyright 2018 IBM
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#pragma once

#include <vector>
#include <memory>
#include <numeric>
#include <experimental/filesystem>
#include "common.h"
#include "pnor_partition_defs.h"

namespace openpower
{
namespace virtual_pnor
{

namespace fs = std::experimental::filesystem;

using PartitionTable = std::vector<uint8_t>;
using checksum_t = uint32_t;

/** @brief Convert the input partition table to big endian.
 *
 *  @param[in] src - reference to the pnor partition table
 *
 *  @returns converted partition table
 */
PartitionTable endianFixup(const PartitionTable& src);

/** @brief Parse a ToC line (entry) into the corresponding FFS partition
 * object.
 *
 * @param[in] line - The ToC line to parse
 * @param[in] blockSize - The flash block size in bytes
 * @param[out] part - The partition object to populate with the information
 *                    parsed from the provided ToC line
 *
 * Throws: MalformedTocEntry, InvalidTocEntry
 */
void parseTocLine(const std::string& line, size_t blockSize,
                  pnor_partition& part);

namespace details
{

/** @brief Compute XOR-based checksum, by XORing consecutive words
 *         in the input data. Input must be aligned to word boundary.
 *
 *  @param[in] data - input data on which checksum is computed
 *
 *  @returns computed checksum
 */
template <class T> checksum_t checksum(const T& data)
{
    static_assert(sizeof(decltype(data)) % sizeof(checksum_t) == 0,
                  "sizeof(data) is not aligned to sizeof(checksum_t) boundary");

    auto begin = reinterpret_cast<const checksum_t*>(&data);
    auto end = begin + (sizeof(decltype(data)) / sizeof(checksum_t));

    return std::accumulate(begin, end, 0, std::bit_xor<checksum_t>());
}

} // namespace details

namespace partition
{

/** @class Table
 *  @brief Generates virtual PNOR partition table.
 *
 *  Generates virtual PNOR partition table upon construction. Reads
 *  the PNOR information generated by this tool :
 *  github.com/openbmc/openpower-pnor-code-mgmt/blob/master/generate-squashfs,
 *  which generates a minimalistic table-of-contents (toc) file and
 *  individual files to represent various partitions that are of interest -
 *  these help form the "virtual" PNOR, which is typically a subset of the full
 *  PNOR image.
 *  These files are stored in a well-known location on the PNOR.
 *  Based on this information, this class prepares the partition table whose
 *  structure is as outlined in pnor_partition.h.
 *
 *  The virtual PNOR supports 4KB erase blocks - partitions must be aligned to
 *  this size.
 */
class Table
{
  public:
    /** @brief Constructor accepting the path of the directory
     *         that houses the PNOR partition files.
     *
     *  @param[in] directory - path of the directory housing PNOR partitions
     *  @param[in] blockSize - PNOR block size, in bytes. See
     *             open-power/hostboot/blob/master/src/usr/pnor/ffs.h for
     *             the PNOR FFS structure.
     *  @param[in] pnorSize - PNOR size, in bytes
     *
     * Throws MalformedTocEntry, InvalidTocEntry
     */
    Table(fs::path&& directory, size_t blockSize, size_t pnorSize);

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) = delete;
    Table& operator=(Table&&) = delete;
    ~Table() = default;

    /** @brief Return the exact size of partition table in bytes
     *
     *  @returns size_t - size of partition table in bytes
     */
    size_t size() const
    {
        return szBytes;
    }

    /** @brief Return aligned size of partition table in bytes
     *
     *  The value returned will be greater-than or equal to size(), and
     *  aligned to blockSize.
     *
     *  @returns size_t - capacity of partition table in bytes
     */
    size_t capacity() const
    {
        return align_up(szBytes, blockSize);
    }

    /** @brief Return the size of partition table in blocks
     *
     *  @returns size_t - size of partition table in blocks
     */
    size_t blocks() const
    {
        return capacity() / blockSize;
    }

    /** @brief Return a partition table having byte-ordering
     *         that the host expects.
     *
     *  The host needs the partion table in big-endian.
     *
     *  @returns const reference to host partition table.
     */
    const pnor_partition_table& getHostTable() const
    {
        return *(reinterpret_cast<const pnor_partition_table*>(hostTbl.data()));
    }

    /** @brief Return a little-endian partition table
     *
     *  @returns const reference to native partition table
     */
    const pnor_partition_table& getNativeTable() const
    {
        return *(reinterpret_cast<const pnor_partition_table*>(tbl.data()));
    }

    /** @brief Return partition corresponding to PNOR offset, the offset
     *         is within returned partition.
     *
     *  @param[in] offset - PNOR offset in bytes
     *
     *  @returns const reference to pnor_partition, if found, else an
     *           exception will be thrown.
     */
    const pnor_partition& partition(size_t offset) const;

    /** @brief Return partition corresponding to input partition name.
     *
     *  @param[in] name - PNOR partition name
     *
     *  @returns const reference to pnor_partition, if found, else an
     *           exception will be thrown.
     */
    const pnor_partition& partition(const std::string& name) const;

  private:
    /** @brief Prepares a vector of PNOR partition structures.
     *
     * Throws: MalformedTocEntry, InvalidTocEntry
     */
    void preparePartitions();

    /** @brief Prepares the PNOR header.
     */
    void prepareHeader();

    /** @brief Allocate memory to hold the partion table. Determine the
     *         amount needed based on the partition files in the toc file.
     *
     *  @param[in] tocFile - Table of contents file path.
     */
    void allocateMemory(const fs::path& tocFile);

    /** @brief Return a little-endian partition table
     *
     *  @returns reference to native partition table
     */
    pnor_partition_table& getNativeTable()
    {
        return *(reinterpret_cast<pnor_partition_table*>(tbl.data()));
    }

    /** @brief Size of the PNOR partition table -
     *         sizeof(pnor_partition_table) +
     *         (no. of partitions * sizeof(pnor_partition)),
     */
    size_t szBytes;

    /** @brief Partition table */
    PartitionTable tbl;

    /** @brief Partition table with host byte ordering */
    PartitionTable hostTbl;

    /** @brief Directory housing generated PNOR partition files */
    fs::path directory;

    /** @brief Number of partitions */
    size_t numParts;

    /** @brief PNOR block size, in bytes */
    size_t blockSize;

    /** @brief PNOR size, in bytes */
    size_t pnorSize;
};
} // namespace partition

/** @brief An exception type storing a reason string.
 *
 *  This looks a lot like how std::runtime_error might be implemented however
 *  we want to avoid extending it, as exceptions extending ReasonedError have
 *  an expectation of being handled (can be predicted and are inside the scope
 *  of the program).
 *
 *  From std::runtime_error documentation[1]:
 *
 *  > Defines a type of object to be thrown as exception. It reports errors
 *  > that are due to events beyond the scope of the program and can not be
 *  > easily predicted.
 *
 *  [1] http://en.cppreference.com/w/cpp/error/runtime_error
 *
 *  We need to keep the inheritance hierarchy separate: This avoids the
 *  introduction of code that overzealously catches std::runtime_error to
 *  handle exceptions that would otherwise derive ReasonedError, and in the
 *  process swallows genuine runtime failures.
 */
class ReasonedError : public std::exception
{
  public:
    ReasonedError(const std::string&& what) : _what(what)
    {
    }
    const char* what() const noexcept
    {
        return _what.c_str();
    };

  private:
    const std::string _what;
};

/** @brief Base exception type for errors related to ToC entry parsing.
 *
 *  Callers of parseTocEntry() may not be concerned with the specifics and
 *  rather just want to extract and log what().
 */
class TocEntryError : public ReasonedError
{
  public:
    TocEntryError(const std::string&& reason) : ReasonedError(std::move(reason))
    {
    }
};

/** @brief The exception thrown on finding a syntax error in the ToC entry
 *
 *  If the syntax is wrong, or expected values are missing, the ToC entry is
 *  malformed
 */
class MalformedTocEntry : public TocEntryError
{
  public:
    MalformedTocEntry(const std::string&& reason) :
        TocEntryError(std::move(reason))
    {
    }
};

/** @brief The exception thrown on finding a semantic error in the ToC entry
 *
 *  If the syntax of the ToC entry is correct but the semantics are broken,
 *  then we have an invalid ToC entry.
 */
class InvalidTocEntry : public TocEntryError
{
  public:
    InvalidTocEntry(const std::string&& reason) :
        TocEntryError(std::move(reason))
    {
    }
};

} // namespace virtual_pnor
} // namespace openpower
