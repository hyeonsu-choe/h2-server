#pragma once

#include <iostream>
#include <memory>

class file_context_t {
	public:
		size_t size;
		int offset;
		const char* data; 

		file_context_t();
		~file_context_t();
};

class MappedFile {
    public:
        void* data;
        size_t data_len;
    public:
        MappedFile(const char* path);
        ~MappedFile();

        const char*  get_data() const;
        const size_t get_data_len() const;

        friend std::ostream& operator<<(std::ostream& os, const MappedFile& map_file)
        {
            char* data = static_cast<char*>(map_file.data);
            os.write(data, map_file.data_len);

            return os;
        }
};

std::shared_ptr<MappedFile> find_file_from_filecache(const char* file_path);
std::shared_ptr<MappedFile> insert_file_into_filecache(const char* file_path);
