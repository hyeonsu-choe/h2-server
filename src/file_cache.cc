#include <sys/mman.h>   // mmap, munmap
#include <sys/stat.h>   // fstat
#include <fcntl.h>      // open
#include <unistd.h>     // close
#include <unordered_map>
#include <string>

#include "file_cache.h"

static thread_local std::unordered_map<std::string, std::shared_ptr<MappedFile>> file_cache;

FileContext::FileContext()
	: size(0), offset(0), data(nullptr)
{

}

FileContext::~FileContext()
{

}

MappedFile::MappedFile(const char* path)
	: data(nullptr), data_len(0)
{
	map(path);
}

MappedFile::~MappedFile()
{
	unmap();
}

bool MappedFile::map(const char* path)
{
	if (path != nullptr) {
		int fd = open(path, O_RDONLY);
		if (fd == -1) {
			std::cerr << "open() error" << std::endl;
			return false;
		}

		struct stat st;
		if (fstat(fd, &st) == -1) {
			close(fd);
			std::cerr << "fstat() error" << std::endl;
			return false;
		}

		data_len = st.st_size;

		data = mmap(NULL, data_len, PROT_READ, MAP_PRIVATE, fd, 0);
		if (data == MAP_FAILED) {
			close(fd);
			std::cerr << "mmap() error : MAP_FAILED" << std::endl;
			return false;
		}

		close(fd); // mmap 이후 fd는 더 이상 필요 없음
		return true;
	}

	return false;
}

void MappedFile::unmap()
{
	if (data) {
		if (munmap(data, data_len) == -1) {
			std::cerr << "munmap" << std::endl;
		}
	}
}

const char*  MappedFile::get_data() const
{
	return static_cast<char*>(data);
}

const size_t MappedFile::get_data_len() const
{
	return data_len;
}

bool MappedFile::is_mapped() const
{
	return get_data() != nullptr;
}

std::shared_ptr<MappedFile> find_file_from_filecache(const std::string& file_path)
{
	auto itr = file_cache.find(file_path);
	if (itr == file_cache.end()) {
		return nullptr;
	}
	return itr->second;
}

std::shared_ptr<MappedFile> insert_file_into_filecache(const std::string& file_path)
{
	try {
		auto result = file_cache.emplace(file_path, std::make_shared<MappedFile>(file_path.c_str()));
		if (!result.second) {
			std::cerr << "failed to emplace" << std::endl;
			return nullptr;	
		}
		return result.first->second;
	} catch (const std::bad_alloc& except) {
		std::cerr << "failed to alloc: " << except.what() << std::endl;
		return nullptr;
	} catch (const std::runtime_error& except) {
		std::cerr << "failed to construct MappedFile: " << except.what() << std::endl;
		return nullptr;
	}
}
