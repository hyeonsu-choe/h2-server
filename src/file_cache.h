#pragma once

#include <iostream>
#include <memory>

class FileContext {
	public:
		size_t size;
		int offset;
		const char* data; 

		FileContext();
		~FileContext();
};

class MappedFile {
    public:
        void* data; // mmap 된 주소 저장
        size_t data_len;

    public:
        MappedFile(const char* path = nullptr);
        ~MappedFile();

		bool map(const char* path);
		void unmap();
        const char*  get_data() const;
        const size_t get_data_len() const;
		bool is_mapped() const;

        friend std::ostream& operator<<(std::ostream& os, const MappedFile& map_file)
        {
            char* data = static_cast<char*>(map_file.data);
            os.write(data, map_file.data_len);

            return os;
        }
};

std::shared_ptr<MappedFile> find_file_from_filecache(std::string_view file_path);
std::shared_ptr<MappedFile> insert_file_into_filecache(std::string_view file_path);
