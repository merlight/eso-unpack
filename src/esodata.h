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
#ifndef ESOUNPACK_ESODATA_H
#define ESOUNPACK_ESODATA_H

#include <endian.h>
#include <stdint.h>

#ifndef PACKED
# define PACKED __attribute__((__packed__))
#endif


struct ESOHostEndianBuffer
{
    ESOHostEndianBuffer(const void* ptr)
      : _ptr(reinterpret_cast<const char*>(ptr))
    {}

    template <typename T>
    T get(size_t ofs) const
    {
        return *reinterpret_cast<const T*>(_ptr + ofs);
    }

    const char* ptr(size_t ofs) const
    {
        return _ptr + ofs;
    }

    int8_t   i8(size_t ofs) const { return get<int8_t>(ofs); }
    int16_t  i16(size_t ofs) const { return get<int16_t>(ofs); }
    int32_t  i32(size_t ofs) const { return get<int32_t>(ofs); }
    uint8_t  u8(size_t ofs) const { return get<uint8_t>(ofs); }
    uint16_t u16(size_t ofs) const { return get<uint16_t>(ofs); }
    uint32_t u32(size_t ofs) const { return get<uint32_t>(ofs); }

    const char* _ptr;
};


struct ESOBigEndianBuffer : ESOHostEndianBuffer
{
    ESOBigEndianBuffer(const void* ptr)
      : ESOHostEndianBuffer(ptr)
    {}

    int16_t  i16(size_t ofs) const { return be16toh(get<int16_t>(ofs)); }
    int32_t  i32(size_t ofs) const { return be32toh(get<int32_t>(ofs)); }
    uint16_t u16(size_t ofs) const { return be16toh(get<uint16_t>(ofs)); }
    uint32_t u32(size_t ofs) const { return be32toh(get<uint32_t>(ofs)); }
};


struct ESOLittleEndianBuffer : ESOHostEndianBuffer
{
    ESOLittleEndianBuffer(const void* ptr)
      : ESOHostEndianBuffer(ptr)
    {}

    int16_t  i16(size_t ofs) const { return le16toh(get<int16_t>(ofs)); }
    int32_t  i32(size_t ofs) const { return le32toh(get<int32_t>(ofs)); }
    uint16_t u16(size_t ofs) const { return le16toh(get<uint16_t>(ofs)); }
    uint32_t u32(size_t ofs) const { return le32toh(get<uint32_t>(ofs)); }
};


struct PACKED ESOBlockType0Header
{
    uint16_t    blockType;
    uint32_t    unknown1;
};


struct PACKED ESOBlockType3Header
{
    uint16_t    blockType;
    uint32_t    fieldSize;
    uint32_t    recordCount[3];

    template <typename BufferT>
    bool init(const BufferT buf, size_t buflen)
    {
        if (buflen < 18) return false;

        this->blockType = buf.u16(0);
        this->fieldSize = buf.u32(2);
        this->recordCount[0] = buf.u32(6);
        this->recordCount[1] = buf.u32(10);
        this->recordCount[2] = buf.u32(14);

        return this->blockType == 3;
    }
};


union PACKED ESOBlockHeader
{
    uint16_t            blockType;
    ESOBlockType0Header headerType0;
    ESOBlockType3Header headerType3;
};


struct PACKED ESOMNFFileHeader
{
    char        mesMagic[4];
    uint16_t    mesVersion;
    uint8_t     datFileCount;
    uint32_t    mnfFileType;
    uint32_t    mnfDataSize;
};


struct PACKED ESOZOSFTHeader
{
    char        headerMagic[5];
    uint16_t    unknown1;
    uint32_t    unknown2;
    uint32_t    unknown3;
    uint32_t    recordCount;
};


struct PACKED ESOZOSFTBlock2Data3Record
{
    uint32_t    fileId;
    uint32_t    filenameOffset;
    uint32_t    filenameHash;
    uint32_t    unknown0x0C;
};


template <typename byteT>
struct ESOSubfileHeader
{
    int32_t     null1;
    int32_t     size1;
    byteT*      data1;
    int32_t     size2;
    byteT*      data2;
    byteT*      filedata;

    int32_t data1_offset() const
    {
        return 8;
    }

    int32_t data2_offset() const
    {
        return 12 + size1;
    }

    int32_t filedata_offset() const
    {
        return 12 + size1 + size2;
    }

    bool init(byteT* bufptr, size_t buflen)
    {
        if (buflen < 8) return false;

        this->null1 = be32toh(*(const int32_t*)(bufptr + 0));
        this->size1 = be32toh(*(const int32_t*)(bufptr + 4));
        this->data1 = (bufptr + 8);

        int32_t size2off = 8 + this->size1;
        if (buflen < size2off + 4) return false;

        this->size2 = be32toh(*(const int32_t*)(bufptr + size2off));
        this->data2 = (bufptr + size2off + 4);

        int32_t fileoff = 12 + this->size1 + this->size2;
        if (buflen < fileoff) return false;

        this->filedata = bufptr + fileoff;
        return true;
    }
};


#endif // ESOUNPACK_ESODATA_H
