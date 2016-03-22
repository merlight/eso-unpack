//==========================================================================
//
//  This is free and unencumbered software released into the public domain.
//
//  Anyone is free to copy, modify, publish, use, compile, sell, or
//  distribute this software, either in source code form or as a compiled
//  binary, for any purpose, commercial or non-commercial, and by any
//  means.
//
//  In jurisdictions that recognize copyright laws, the author or authors
//  of this software dedicate any and all copyright interest in the
//  software to the public domain. We make this dedication for the benefit
//  of the public at large and to the detriment of our heirs and
//  successors. We intend this dedication to be an overt act of
//  relinquishment in perpetuity of all present and future rights to this
//  software under copyright law.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
//  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
//  OTHER DEALINGS IN THE SOFTWARE.
//
//  For more information, please refer to <http://unlicense.org/>
//
//==========================================================================
#define ZLIB_CONST

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include "esodata.h"
#include "fileio.h"


#define logf(args...) fprintf(stderr, args)

extern "C" uint32_t hash(const char* k, uint32_t length, uint32_t initval);


static bool optSaveSubfiles = false;
static std::string optEsoDir = ".";
static std::string optOutDir = "game.unpacked";


static bool is_valid_path(const char* start, const char* end)
{
    for (const char* s = start; s != end; ++s) {
        if (*s == '\0') {
            return s != start;
        }
        if (!isalnum(*s) && !strchr(" +-./_", *s)) {
            return false;
        }
    }
    return false;
}


static bool startswith(const char* str, const char* prefix)
{
    return std::memcmp(str, prefix, std::strlen(prefix)) == 0;
}


static bool startswith(std::string str, const char* prefix)
{
    return std::memcmp(str.c_str(), prefix, std::strlen(prefix)) == 0;
}


static bool endswith(const std::string& str, char c)
{
    size_t len = str.size();
    return len >= 1 && str[len - 1] == c;
}


// several reasons this is evil:
// 1) it modifies the path string in place during traversal
// 2) it is susceptible to filesystem race conditions
// 3) I don't care, I need it fast ^^
static void make_path(char* path)
{
    for (char* s = path; *s != '\0'; ++s) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(path, 0755) == 0) {
                std::clog << "created directory " << path << std::endl;
            }
            *s = '/';
        }
    }
}


static std::string outputFilename(const char* outdir, size_t offset, const char* ext)
{
    char fn[200];
    int sub1 = (offset >> 24) & 0xff;
    int sub2 = (offset >> 16) & 0xff;
    int len = snprintf(fn, sizeof(fn), "%s/%02x/%02x/%08lx%s", outdir, sub1, sub2, offset, ext);
    return std::string(fn, len < sizeof(fn) ? len : sizeof(fn));
}


static std::string outputPathFromOffset(size_t offset, const char* ext)
{
    char fn[200];
    int sub1 = (offset >> 20) & 0xfff;
    int len = snprintf(fn, sizeof(fn), "%03x/%08lx%s", sub1, offset, ext);
    return std::string(fn, len < sizeof(fn) ? len : sizeof(fn));
}


struct SubfileInfo
{
    uint32_t    fileId;
    uint32_t    _maybe_flags1;
    uint32_t    compressedSize;
    uint32_t    uncompressedSize;
    uint32_t    _maybe_contentHash;
    uint32_t    fileOffset;
    uint32_t    _maybe_flags2;
};

static std::vector<SubfileInfo> g_subfiles;


static const char* filetypeHeuristics(const std::string& filedata)
{
    size_t head_len = filedata.size();
    const char* head_buf = filedata.c_str();

    if (head_len >= 8 && !std::memcmp(head_buf, "DDS\x20\x7c\x00\x00\x00", 8)) {
        return "textures/.dds";
    }
    if (head_len >= 4 && !std::memcmp(head_buf, "OTTO", 4)) {
        return "fonts/.otf";
    }
    if (head_len >= 5 && !std::memcmp(head_buf, "ZOSFT", 5)) {
        return "zosft/.zosft";
    }
    return ".unk";
}


static bool inflateString(const char* in_buf, size_t in_len,
                          char* out_buf, size_t* out_len)
{
    z_stream zs;
    zs.next_in = (const Bytef*)in_buf;
    zs.avail_in = in_len;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;

    if (inflateInit(&zs) != Z_OK) {
        return false;
    }

    zs.next_out = (Bytef*)out_buf;
    zs.avail_out = *out_len;

    int zerr = inflate(&zs, Z_FINISH);
    *out_len = zs.next_out - (Bytef*)out_buf;

    if (inflateEnd(&zs) != Z_OK) {
        logf("zlib: %s\n", zs.msg);
    }

    return zerr == Z_STREAM_END;
}


static std::vector<std::string> g_heuristicsResults;
static std::vector<ssize_t> g_fileOffsets;

static bool tryInflate(const std::string& outdir, TBuffer<Bytef>& in_buf,
                       std::string& out_data)
{
    size_t startOffset = in_buf.offset();
    Bytef* in_ptr;
    size_t in_size;
    char out_buf[8000];
    z_stream zs;

    out_data.clear();
    in_buf.next(8000, &in_size, &in_ptr);

    zs.next_in = in_ptr;
    zs.avail_in = in_size;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;

    if (inflateInit(&zs) != Z_OK) {
        return false;
    }

    // ensure inflateEnd(zs) gets called upon exception / return
    std::unique_ptr<z_stream, int (&)(z_streamp)> zsGuard(&zs, inflateEnd);

    while (true) {

        zs.next_out = (Bytef*)out_buf;
        zs.avail_out = sizeof(out_buf);

        int zerr = inflate(&zs, Z_NO_FLUSH);
        size_t out_len = zs.next_out - (Bytef*)out_buf;

        if (zs.next_in > in_ptr) {
            in_buf.consume(zs.next_in - in_ptr);
            in_buf.next(8000, &in_size, &in_ptr);
            zs.next_in = in_ptr;
            zs.avail_in = in_size;
        }

        if (out_len > 0) {
            out_data.append(out_buf, out_len);
        }

        if (zerr == Z_STREAM_END) {
            // done
            break;
        }

        if (zerr != Z_OK) {
            // error
            std::cerr << "inflate failed at " << in_buf.offset()
                      << " with error " << zerr << std::endl;
            return false;
        }
    }

    zsGuard.reset();

    ESOSubfileHeader<const char> hdr = ESOSubfileHeader<const char>();
    const char* hfn = 0;

    if (hdr.init(out_data.c_str(), out_data.size())) {
        out_data.erase(0, hdr.filedata_offset());
        hfn = filetypeHeuristics(out_data);
    }

    g_fileOffsets.push_back(startOffset);
    g_heuristicsResults.push_back(hfn ? hfn : "");

    std::string outfile = outdir;
    outfile.append(!endswith(outfile, '/'), '/');
    outfile.append(outputPathFromOffset(startOffset, ".raw"));

    snprintf(out_buf, sizeof(out_buf),
             "[%04lx] extracted file from offset %08lx ( %08lx %08lx %08lx ) heuristics: %s",
             g_fileOffsets.size() - 1,
             startOffset, startOffset - 14,
             startOffset + hdr.filedata_offset(),
             startOffset - 14 + hdr.filedata_offset(),
             hfn ? hfn : "null");
    std::cout << out_buf << std::endl;

    //std::clog << "writing file " << outfile << std::endl;
    if (optSaveSubfiles) {
        make_path(&outfile[0]);

        File fw;
        fw.open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        fw.write(out_data.data(), out_data.size());
    }

    return true;
}


static void dumpZOSFT(const std::string& outdir, const std::string& zosft)
{
    const char* ptr = zosft.data();
    size_t size = zosft.size();

    if (size < sizeof(ESOZOSFTHeader)) return;

    const ESOZOSFTHeader* p_hdr = reinterpret_cast<const ESOZOSFTHeader*>(ptr);
    size_t offset = sizeof(ESOZOSFTHeader);

    std::cout << "ZOSFT dump";
    std::cout << "\nrecordCount: " << p_hdr->recordCount;

    ESOBlockType3Header blockHeaders[3];
    struct DataBlockInfo {
        size_t offset;
        size_t uncompressedSize;
        size_t compressedSize;
        std::string uncompressedData;
    } dataBlocks[3][3];

    for (int bi = 0; bi < 3; ++bi) {
        ESOBlockType3Header& bh = blockHeaders[bi];
        if (!bh.init(ESOLittleEndianBuffer(ptr + offset), size - offset)) return;
        offset += sizeof(bh);
        std::cout << "\nblock " << (bi + 1) << " type: " << bh.blockType
                  << "\n        fieldSize: " << bh.fieldSize
                  << "\n        recordCount1: " << bh.recordCount[0]
                  << "\n        recordCount2: " << bh.recordCount[1]
                  << "\n        recordCount3: " << bh.recordCount[2];
        for (int di = 0; di < 3; ++di) {
            uint32_t recordCount = bh.recordCount[di];
            DataBlockInfo& dh = dataBlocks[bi][di];
            dh.offset = (offset + 8);
            if (dh.offset > size) return;
            if (recordCount == 0) {
                dh.compressedSize = 0;
                dh.uncompressedSize = 0;
                continue;
            }
            dh.uncompressedSize = le32toh(*(const uint32_t*)(ptr + offset));
            dh.compressedSize = le32toh(*(const uint32_t*)(ptr + offset + 4));
            std::cout << "\n        data " << (di + 1) << " uncompressedSize: " << dh.uncompressedSize
                      << " = " << recordCount << " * " << ((double)dh.uncompressedSize / recordCount)
                      << "\n               compressedSize: " << dh.compressedSize;
            offset += 8 + dh.compressedSize;
            if (offset > size) return;
        }
    }

    for (int bi = 0; bi < 3; ++bi) {
        for (int di = 0; di < 3; ++di) {
            DataBlockInfo& dh = dataBlocks[bi][di];
            size_t recordCount = blockHeaders[bi].recordCount[di];
            if (recordCount == 0)
                continue;
            size_t uncompressedSize = dh.uncompressedSize;
            size_t recordSize = uncompressedSize / recordCount;
            dh.uncompressedData.reserve(uncompressedSize + 4); // make some room for reading dwords
            dh.uncompressedData.resize(uncompressedSize, '.');
            bool ok = inflateString(ptr + dh.offset, dh.compressedSize,
                                    &dh.uncompressedData[0], &uncompressedSize);
            std::cout << "\nblock " << (bi + 1) << " data " << (di + 1);
            if (!ok) {
                std::cout << " decompression failed!";
                continue;
            }
            if (dh.uncompressedData.size() != uncompressedSize) {
                std::cout << " decompressed size mismatch: "
                          << uncompressedSize << " != " << dh.uncompressedData.size();
            }
            else {
                std::cout << " decompressed size: " << uncompressedSize;
            }
            dh.uncompressedData.resize(uncompressedSize, '.');
            const char* newline = "\n               ";
            size_t nonzeroCount = 0;
            std::set<uint32_t> uniqueValues;
            for (int k = 0; k < uncompressedSize / 4; ++k) {
                uint32_t v = *(const uint32_t*)(dh.uncompressedData.data() + 4 * k);
                nonzeroCount += (v != 0);
                uniqueValues.insert(v);
                char tmp[100];
                int recPerLine = 1;
                if (recordSize <= 8) {
                    recPerLine = 4;
                }
                else if (recordSize <= 16) {
                    recPerLine = 2;
                }
                if ((4 * k) % (recPerLine * recordSize) == 0) {
                    snprintf(tmp, 100, "%s[%04lx] %08x", newline, (4 * k) / recordSize, v);
                }
                else if ((4 * k) % recordSize == 0) {
                    snprintf(tmp, 100, "%4s[%04lx] %08x", "", (4 * k) / recordSize, v);
                }
                else {
                    snprintf(tmp, 100, " %08x", v);
                }
                std::cout << tmp;
            }
            std::cout << newline << "# of nonzero values: " << nonzeroCount;
            std::cout << newline << "# of unique values: " << uniqueValues.size();
            std::set<uint32_t>::iterator it;
            it = uniqueValues.lower_bound(0x80000000);
            if (it != uniqueValues.end()) {
                uint32_t v1 = *it - 0x80000000;
                uint32_t v2 = *--(it = uniqueValues.end()) - 0x80000000;
                std::cout << newline << "min nonzero value: 0x80000000 + " << v1;
                std::cout << newline << "max nonzero value: 0x80000000 + " << v2;
            }
        }
    }

    using B2D3 = ESOZOSFTBlock2Data3Record;

    const uint32_t* block0data0 = (const uint32_t*)dataBlocks[0][0].uncompressedData.data();
    const uint32_t* block1data0 = (const uint32_t*)dataBlocks[1][0].uncompressedData.data();
    const uint32_t* block1data2 = (const uint32_t*)dataBlocks[1][2].uncompressedData.data();
    const B2D3* block2data3 = (const B2D3*)dataBlocks[1][2].uncompressedData.data();
    size_t block0data0n = dataBlocks[0][0].uncompressedData.size() / 4;
    size_t block1data0n = dataBlocks[1][0].uncompressedData.size() / 4;
    size_t block1data2n = dataBlocks[1][2].uncompressedData.size() / 4;
    size_t block2data3n = dataBlocks[1][2].uncompressedData.size() / sizeof(B2D3);

    for (size_t i = 0; i < blockHeaders[0].recordCount[0]; ++i) {
        char tmp[200];
        tmp[0] = '\0';
        if (i % 32 == 0) {
            sprintf(tmp, "\n%-17s%-18s%s", "data 1", "block 1", "block 2");
        }
        if (i < block0data0n) {
            sprintf(tmp, "%s\n%-10s[%04lx] %08x", tmp, "", i, block0data0[i]);
        }
        else {
            sprintf(tmp, "%s\n%-25s", tmp, "");
        }
        if (i < block1data0n) {
            sprintf(tmp, "%s%-3s[%04lx] %08x", tmp, "", i, block1data0[i]);
        }
        else {
            sprintf(tmp, "%s%18s", tmp, "");
        }
        std::cout << tmp;
    }

    if (offset + 4 > size) return;

    size_t fndataSize = le32toh(*(const uint32_t*)(ptr + offset));
    std::cout << "\n======================================================================";
    std::cout << "\nFILENAMES data size: " << fndataSize;
    std::cout << "\nNUM  OFFSET   HASH     FILENAME";

    const char* filenames = ptr + (offset += 4);
    const char* filenamesEnd = filenames + std::min(fndataSize, size - offset);
    const char* name = filenames;
    size_t filenameIndex = 0;

    for (const char* s = filenames; s < filenamesEnd; ++s) {
        if (*s == '\0') {
            if (name < s) {
                char tmp[300];
                uint32_t h = hash(name, s - name, 0xa8396u);
                snprintf(tmp, sizeof(tmp), "\n%04lx %08lx %08x %s",
                         filenameIndex++, name - filenames, h, name);
                std::cout << tmp;
            }
            name = s + 1;
        }
    }

    std::cout << std::endl;

    for (size_t i = 0; i < g_fileOffsets.size(); ++i) {
        ssize_t startOffset = g_fileOffsets.at(i);
        const std::string& heur = g_heuristicsResults.at(i);
        const char* filename = 0;
        uint32_t fileId = 0;

        for (auto const& info : g_subfiles) {
            if (startOffset == info.fileOffset) {
                fileId = info.fileId;
                break;
            }
        }

        logf("offset %08lx fileId %04x\n", startOffset, fileId);

        for (size_t j = 0; fileId && j < block2data3n; ++j) {
            size_t ofs = block2data3[j].filenameOffset;
            if (fileId == block2data3[j].fileId
                && ofs < filenamesEnd - filenames
                && (ofs == 0 || filenames[ofs - 1] == '\0')
                && is_valid_path(filenames + ofs, filenamesEnd)) {
                filename = filenames + ofs;
                logf("ZOSFT name found for file at offset %08lx : %s\n", startOffset, filename);
            }
        }

        std::string oldpath = outdir;
        oldpath.append(!endswith(oldpath, '/'), '/');
        oldpath.append(outputPathFromOffset(startOffset, ".raw"));

        std::string newpath = outdir;
        std::string reason;
        newpath.append(!endswith(newpath, '/'), '/');

        if (filename) {
            reason = "ZOSFT filename";
            newpath.append(filename);
        }
        else if (!heur.empty()) {
            const char* dirsep = strrchr(heur.c_str(), '/');
            const char* basename = (dirsep ? dirsep + 1 : heur.c_str());
            if (basename[0] == '.') {
                newpath.append(heur.c_str(), basename);
                newpath.append(outputPathFromOffset(startOffset, basename));
            }
            else {
                newpath.append(heur);
            }
            reason = "heuristics " + heur;
        }
        else {
            continue;
        }

        std::clog << "renaming " << oldpath << " to " << newpath
                  << " (" << reason << ")" << std::endl;
        if (optSaveSubfiles) {
            make_path(&newpath[0]);
            rename(oldpath.c_str(), newpath.c_str());
        }
    }
}


static void readMNF(const char* path)
{
    FileMapping fr(path);

    if (fr.size() < sizeof(ESOMNFFileHeader)) {
        fr.error("file header truncated");
    }

    const ESOMNFFileHeader* p_hdr = reinterpret_cast<const ESOMNFFileHeader*>(fr.data());

    std::clog <<   " MES magic:      " << std::string(p_hdr->mesMagic, 4)
              << "\n MES version:    " << p_hdr->mesVersion
              << "\n DAT file count: " << (int)p_hdr->datFileCount
              << "\n MNF file type:  " << p_hdr->mnfFileType
              << "\n MNF data size:  " << p_hdr->mnfDataSize
              << "\n";

    off_t offset = sizeof(ESOMNFFileHeader);
    int blockCount = 0;

    while (offset < fr.size()) {
        if (fr.size() - offset < 2) {
            fr.error("block header truncated");
        }
        ESOBigEndianBuffer blkbuf(fr.data() + offset);
        uint16_t blockType = blkbuf.u16(0);

        blockCount++;
        std::clog << " block #" << blockCount << " type " << blockType << "\n";
        if (blockType == 3) {
            char strbuf[200];
            ESOBlockType3Header bh;
            bh.init(blkbuf, fr.size() - offset);
            logf("  field size:     %6d\n", bh.fieldSize);
            logf("  record count #1:%6d\n", bh.recordCount[0]);
            logf("  record count #2:%6d\n", bh.recordCount[1]);
            logf("  record count #3:%6d\n", bh.recordCount[2]);
            size_t subfileCount = bh.recordCount[2];
            size_t dataOffset = sizeof(bh);
            std::string uncompressedData;
            g_subfiles.resize(subfileCount);
            for (int di = 1; di <= 3; ++di) {
                size_t uncompressedSize = blkbuf.u32(dataOffset);
                size_t compressedSize = blkbuf.u32(dataOffset + 4);
                size_t compressedDataOffset = dataOffset + 8;
                size_t recordCount = bh.recordCount[di - 1];
                size_t numCols = 1;
                logf("  data size #%d:   %6ld  uncompressed: %6ld", di, compressedSize, uncompressedSize);
                if (recordCount > 0) {
                    double recordSize = uncompressedSize / (double)recordCount;
                    logf("  record size: %4.1f", recordSize);
                    numCols = (recordSize == 4.0 ? 4 : recordSize == 8.0 ? 2 : 1);
                }
                logf("  pos: %08lx\n", offset + dataOffset + 8);
                dataOffset += 8 + compressedSize;

                uncompressedData.reserve(uncompressedSize + 4); // make some room for reading dwords
                uncompressedData.resize(uncompressedSize, '.');
                bool ok = inflateString(blkbuf.ptr(compressedDataOffset), compressedSize,
                                        &uncompressedData[0], &uncompressedSize);

                std::cout << "\nblock #" << blockCount << " data #" << di;
                if (!ok) {
                    std::cout << " decompression failed!";
                    continue;
                }
                if (uncompressedData.size() != uncompressedSize) {
                    std::cout << " decompressed size mismatch: "
                              << uncompressedSize << " != " << uncompressedData.size();
                }
                else {
                    std::cout << " decompressed size: " << uncompressedSize;
                }
                uncompressedData.resize(uncompressedSize, '.');

                ESOLittleEndianBuffer databuf(uncompressedData.data());

                for (size_t fi = 0; di == 2 && fi < subfileCount; ++fi) {
                    size_t ofs = fi * 8;
                    if (ofs + 8 > uncompressedSize) {
                        break;
                    }
                    SubfileInfo& info = g_subfiles.at(fi);
                    info.fileId = databuf.u32(ofs + 0);
                    info._maybe_flags1 = databuf.u32(ofs + 4);
                }

                for (size_t fi = 0; di == 3 && fi < subfileCount; fi++) {
                    size_t ofs = fi * 20;
                    if (ofs + 20 > uncompressedSize) {
                        break;
                    }
                    SubfileInfo& info = g_subfiles.at(fi);
                    info.compressedSize = databuf.u32(ofs + 0);
                    info.uncompressedSize = databuf.u32(ofs + 4);
                    info._maybe_contentHash = databuf.u32(ofs + 8);
                    info.fileOffset = databuf.u32(ofs + 12);
                    info._maybe_flags2 = databuf.u32(ofs + 16);
                }

                char rec[200];
                for (size_t ri = 0, rn = (uncompressedSize / 4 + numCols - 1) / numCols;
                     di == 1 && ri < rn; ++ri) {
                    snprintf(rec, sizeof(rec), "\n");
                    for (size_t ci = 0; ci < numCols; ++ci) {
                        size_t ofs = (ri + rn * ci) * 4;
                        if (ofs + 4 <= uncompressedSize) {
                            auto len = std::strlen(rec);
                            snprintf(rec + len, sizeof(rec) - len, "  [%04lx]  %08x",
                                     ofs / 4, databuf.u32(ofs));
                        }
                    }
                    std::cout << rec;
                }
                for (size_t ri = 0, rn = (uncompressedSize / 8 + numCols - 1) / numCols;
                     di == 2 && ri < rn; ++ri) {
                    snprintf(rec, sizeof(rec), "\n");
                    for (size_t ci = 0; ci < numCols; ++ci) {
                        size_t ofs = (ri + rn * ci) * 8;
                        if (ofs + 8 <= uncompressedSize) {
                            auto len = std::strlen(rec);
                            snprintf(rec + len, sizeof(rec) - len, "  [%04lx]  %08x  %08x",
                                     ofs / 8, databuf.u32(ofs), databuf.u32(ofs + 4));
                        }
                    }
                    std::cout << rec;
                }
                for (size_t ofs = 0; di == 3 && ofs < uncompressedSize; ofs += 20) {
                    uint32_t datUncompressedSize = databuf.u32(ofs);
                    uint32_t datCompressedSize = databuf.u32(ofs + 4);
                    uint32_t datFileHash = databuf.u32(ofs + 8);
                    uint32_t datFileOffset = databuf.u32(ofs + 12);
                    uint32_t datFileInfo = databuf.u32(ofs + 16);
                    snprintf(rec, sizeof(rec),
                             "\n  [%04lx]  def %08x  inf %08x  hash %08x  ofs %08x  info %08x",
                             ofs / 20, datCompressedSize, datUncompressedSize,
                             datFileHash, datFileOffset, datFileInfo);
                    std::cout << rec;
                }
                std::cout << "\n";
            }
            offset += dataOffset;
            logf("  end pos %08lx\n", offset);
        }
        else {
            fr.error("unknown block type");
        }
    }
}


struct opt_t
{
    const char*     lname;
    bool*           boolval;
    std::string*    strval;
};


static opt_t g_opts[] = {
    { "--esodir", NULL, &optEsoDir },
    { "--outdir", NULL, &optOutDir },
    { "--save", &optSaveSubfiles, NULL },
    { NULL }, // guard
};


static int parseopts(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        for (const opt_t* opt = g_opts; opt->lname; ++opt) {
            auto len = std::strlen(opt->lname);

            if (std::strncmp(arg, opt->lname, len)) {
                continue;
            }
            if (arg[len] == '=') {
                if (opt->boolval) {
                    std::cerr << "boolean option " << opt->lname << " cannot have value" << std::endl;
                    return 2;
                }
                else if (opt->strval) {
                    opt->strval->assign(arg + len + 1);
                }
                goto NEXT_ARG;
            }
            else if (arg[len] == '\0') {
                if (opt->boolval) {
                    *opt->boolval = true;
                }
                else if (opt->strval) {
                    if (++i >= argc) {
                        std::cerr << "missing argument for " << opt->lname << std::endl;
                        return 2;
                    }
                    opt->strval->assign(argv[i]);
                }
                goto NEXT_ARG;
            }
        }

        std::cerr << "unknown option: " << argv[i] << std::endl;
        return 2;

        NEXT_ARG: continue;
    }

    return 0;
}


int main(int argc, char** argv)
{
    try {
        int res = parseopts(argc, argv);
        if (res)
            return res;
    }
    catch (std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }

    try {
        std::string game_mnf = optEsoDir + "/game/client/game.mnf";
        std::clog << "reading " << game_mnf << std::endl;
        readMNF(game_mnf.c_str());
    }
    catch (std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
    }

    try {
        std::string game_dat = optEsoDir + "/game/client/game0000.dat";
        std::clog << "reading " << game_dat << "\n";
        File frdata(game_dat, O_RDONLY);
        TBuffer<Bytef> in_buf(256000, frdata);
        Bytef* in_ptr;
        std::string out_data;
        size_t in_size;
        while (in_buf.next(8000, &in_size, &in_ptr)) {
            size_t z_pos = 0;
            for (; z_pos < in_size; ++z_pos) {
                Bytef CompressionMethodAndFlags = in_ptr[z_pos];
                Bytef CompressionMethod = CompressionMethodAndFlags & 0x0f;
                if (CompressionMethod != Z_DEFLATED) {
                    continue;
                }
                Bytef CompressionInfo = (CompressionMethodAndFlags >> 4) & 0x0f;
                if (CompressionInfo > 7) {
                    continue;
                }
                break;
            }
            in_buf.consume(z_pos);
            if (z_pos < in_size) {
                //std::clog << "found possible zlib block at " << in_buf.offset(z_pos) << std::endl;
                bool good = tryInflate(optOutDir, in_buf, out_data);
                if (!good) {
                    in_buf.consume(1);
                }
                else if (!out_data.compare(0, 5, "ZOSFT") &&
                         !out_data.compare(out_data.size() - 5, 5, "ZOSFT")) {
                    dumpZOSFT(optOutDir, out_data);
                    std::cout << std::endl;
                }
            }
        }
    }
    catch (std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
