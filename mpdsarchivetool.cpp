#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <fstream>
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <string>
#include <vector>
#include <exception>
#include "compression.h"

bool MakeDirectory(const char *dir)
{
    int ret;
#if defined(_WIN32)
    ret = _mkdir(dir);
#else 
    ret = mkdir(dir, 0777); // notice that 777 is different than 0777
#endif]
    return ret != -1 || errno == EEXIST;
}

char *ReadDataFile(const char *path, int &size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    char *buffer = (char *)malloc(size);
    fseek(file, 0, SEEK_SET);
    int nbytes = fread(buffer, 1, size, file);
    fclose(file);
    if (size != nbytes) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

struct InputFile {

    InputFile() = default;

    InputFile(const InputFile &other) {
        m_path = other.m_path;
        m_compression_type = other.m_compression_type;
        m_compresssed_size = other.m_compresssed_size;
        m_compressed_buffer = (char *)malloc(m_compresssed_size);
        memcpy(m_compressed_buffer, other.m_compressed_buffer, m_compresssed_size);
    }

    ~InputFile() {
        CleanupBuffer();
    }

    void CleanupBuffer()
    {
        if (m_compressed_buffer) {
            free(m_compressed_buffer);
            m_compressed_buffer = NULL;
        }
        m_compresssed_size = 0;
    }

    bool SetFileInfo(std::string path, int compression_type)
    {
        if (m_path != path || m_compression_type != compression_type) {
            CleanupBuffer();
            int raw_size;
            char *raw_buffer = ReadDataFile(path.c_str(), raw_size);
            if (raw_buffer == NULL) {
                return false;
            }
            m_compressed_buffer = compress(raw_buffer, raw_size, compression_type, &m_compresssed_size);
            free(raw_buffer);
        }
        m_path = path;
        m_compression_type = compression_type;
        return true;
    }

    std::string m_path;
    int m_compression_type = COMPRESSION_NONE;
    char *m_compressed_buffer = NULL;
    int m_compresssed_size = 0;
};

void WriteMemoryBufU32(uint8_t *buf, uint32_t value)
{
    //Split value into bytes in little-endian order
    buf[3] = value >> 24;
    buf[2] = (value >> 16) & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
    buf[0] = value & 0xFF;
}

void WriteFileU32(FILE *file, uint32_t value)
{
    //Convert uint32_t to 4 bytes
    uint8_t temp[4];
    WriteMemoryBufU32(temp, value);
    //Write it to file
    fwrite(temp, 4, 1, file);
}

void RoundUpU32(uint32_t &value, uint32_t to)
{
    value = ((value + to - 1) / to) * to;
}

void PadFile(FILE *file, uint32_t to, uint8_t value)
{
    uint32_t ofs = ftell(file);
    while (ofs % to > 0) {
        //Write padding value until file is aligned to a multiple of to bytes
        fwrite(&value, 1, 1, file);
        ofs++;
    }
}


bool RebuildArchive(std::string in_name, std::string out_name)
{
    int archive_compress_type = COMPRESSION_NONE;
    std::vector<InputFile> input_files;
    std::ifstream in_file(in_name);
    if (!in_file.is_open()) {
        std::cout << "Failed to open " << in_name << " for reading." << std::endl;
        return false;
    }
    std::string line;
    std::getline(in_file, line);
    archive_compress_type = getCompressionTypeId(line.c_str());
    std::string list_base_path = in_name.substr(0, in_name.find_last_of("\\/") + 1);
    while (std::getline(in_file, line)) {
        if (line.find("COMPRESSION") == 0) {
            //Separate out compression format
            size_t comma_pos = line.find_first_of(",");
            if (comma_pos == -1) {
                continue;
            }
            int compression_type = getCompressionTypeId(line.substr(0, comma_pos).c_str());
            //Read input file
            std::string path = list_base_path+line.substr(comma_pos + 1);
            InputFile input_file;
            if (!input_file.SetFileInfo(path, compression_type)) {
                std::cout << "Failed to open " << path << "." << std::endl;
                return false;
            }
            input_files.push_back(input_file);
        }
    }
    in_file.close();
    FILE *out_file = fopen(out_name.c_str(), "wb");
    if (!out_file) {
        std::cout << "Failed to open " << out_file << " for writing." << std::endl;
        return false;
    }
    //Write archive header
    uint32_t file_ofs = input_files.size() * 8;
    WriteFileU32(out_file, input_files.size());
    for (uint32_t i = 0; i < input_files.size(); i++) {
        WriteFileU32(out_file, file_ofs);
        WriteFileU32(out_file, input_files[i].m_compresssed_size);
        file_ofs += input_files[i].m_compresssed_size;
        RoundUpU32(file_ofs, 4);
    }
    //Write file data
    for (uint32_t i = 0; i < input_files.size(); i++) {
        fwrite(input_files[i].m_compressed_buffer, 1, input_files[i].m_compresssed_size, out_file);
        PadFile(out_file, 4, 0);
    }
    fclose(out_file);
    //Compress the archive
    {
        int archive_size = 0;
        char *archive_raw = ReadDataFile(out_name.c_str(), archive_size);
        int archive_size_compressed;
        char *archive_compressed = compress(archive_raw, archive_size, archive_compress_type, &archive_size_compressed);
        free(archive_raw);
        out_file = fopen(out_name.c_str(), "wb");
        fwrite(archive_compressed, 1, archive_size_compressed, out_file);
        PadFile(out_file, 4, 0);
        free(archive_compressed);
        fclose(out_file);
    }
    
    return true;
}

bool ExtractArchive(std::string in_name, std::string out_name)
{
    int archive_size;
    char *archive_buf;
    int archive_comp_type;
    {
        int in_size;
        char *in_buf = ReadDataFile(in_name.c_str(), in_size);
        if (!in_buf) {
            std::cout << "Failed to read " << in_name << "." << std::endl;
            return false;
        }
        archive_comp_type = getCompressionType(in_buf, in_size);
        archive_buf = decompress(in_buf, in_size, &archive_size);
        free(in_buf);
    }
    std::ofstream out_file(out_name);
    if (!out_file.is_open()) {
        std::cout << "Failed to open " << out_name << " for writing." << std::endl;
        return false;
    }
    size_t dot_pos = out_name.find_last_of(".");
    std::string dest_dir = out_name.substr(0, dot_pos) + "/";
    std::string subdir_name;
    size_t last_slash_pos = out_name.find_last_of("\\/");
    subdir_name = out_name.substr(last_slash_pos+1, dot_pos- last_slash_pos-1) + "/";
    if (!MakeDirectory(dest_dir.c_str())) {
        std::cout << "Failed to create " << dest_dir << "." << std::endl;
        out_file.close();
        return false;
    }
    out_file << getCompressionTypeName(archive_comp_type) << std::endl << std::endl;
    uint32_t *archive_data = (uint32_t *)archive_buf;
    uint32_t num_files = *archive_data;
    for (uint32_t i = 0; i < num_files; i++) {
        uint32_t start = archive_data[(i * 2) + 1] + 4;
        uint32_t end;
        if (i == num_files - 1) {
            end = archive_size;
        } else {
            end = archive_data[(i * 2) + 3] + 4;
        }
        uint32_t size = end - start;
        std::string filename = std::to_string(i) + ".bin";
        std::string path = dest_dir + filename;
        int compression_type = getCompressionType(&archive_buf[start], size);
        out_file << getCompressionTypeName(compression_type) << "," << subdir_name+filename << std::endl;
        int raw_size;
        char *raw_buf = decompress(&archive_buf[start], size, &raw_size);
        FILE *file = fopen(path.c_str(), "wb");
        if (!file) {
            std::cout << "Failed to open " << path << " for writing." << std::endl;
            out_file.close();
            free(archive_buf);
            return false;
        }

        fwrite(raw_buf, 1, raw_size, file);
        fclose(file);
        free(raw_buf);
    }
    out_file.close();
    free(archive_buf);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        std::cout << "Invalid number of arguments" << std::endl;
        std::cout << "Usage: " << argv[0] << " in [out]" << std::endl;
        std::cout << "The out parameter is optional" << std::endl;
        return 1;
    }
    bool rebuild;
    std::string in_name = argv[1];
    std::string out_name;
    if (argc == 3) {
        out_name = argv[2];
    }
    if (in_name.rfind(".bin") != std::string::npos) {
        rebuild = false;
        if (argc != 3) {
            out_name = in_name.substr(0, in_name.find_last_of(".")) + ".lst";
        }
    } else {
        rebuild = true;
        if (argc != 3) {
            out_name = in_name.substr(0, in_name.find_last_of(".")) + ".bin";
        }
    }
    if (rebuild) {
        return !RebuildArchive(in_name, out_name);
    } else {
        return !ExtractArchive(in_name, out_name);
    }
}