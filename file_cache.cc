#include <sys/mman.h>   // mmap, munmap
#include <sys/stat.h>   // fstat
#include <fcntl.h>      // open
#include <unistd.h>     // close
#include <unordered_map>
#include <string>

#include "file_cache.h"

static std::unordered_map<std::string, std::shared_ptr<MappedFile>> file_cache;

file_context_t::file_context_t()
	: size(0), offset(0), data(nullptr)
{

}

file_context_t::~file_context_t()
{

}

MappedFile::MappedFile(const char* path)
	: data(nullptr), data_len(0)
{
	if (path != nullptr) {
		int fd = open(path, O_RDONLY);
		if (fd == -1) {
			printf("open()\n");
			//perror("open");
			return;
		}

		struct stat st;
		if (fstat(fd, &st) == -1) {
			perror("fstat");
			close(fd);
			return;
		}

		data_len = st.st_size;

		data = mmap(NULL, data_len, PROT_READ, MAP_PRIVATE, fd, 0);
		if (data == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return;
		}

		//madvise(data, data_len, MADV_WILLNEED);
		close(fd); // mmap 이후 fd는 더 이상 필요 없음
	}
}

MappedFile::~MappedFile()
{
	if (data) {
		if (munmap(data, data_len) == -1) {
			perror("munmap");
		}
		//  std::cout << __func__ << std::endl;
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

std::shared_ptr<MappedFile> find_file_from_filecache(const char* file_path)
{
	auto itr = file_cache.find(file_path);
	if (itr == file_cache.end()) {
		return nullptr;
	}

	return itr->second;
}

//std::pair<std::shared_ptr<MappedFile>, bool> insert_file_into_filecache(const char* file_path)
std::shared_ptr<MappedFile> insert_file_into_filecache(const char* file_path)
{
//	std::pair<std::shared_ptr<MappedFile>, bool> ret(nullptr, false);
	try {
		auto result = file_cache.emplace(file_path, std::make_shared<MappedFile>(file_path));
		if (!result.second) {
			std::cerr << "failed to emplace" << std::endl;
			return nullptr;	
		}

		//return std::pair<std::shared_ptr<MappedFile>, bool>(result.first->second, true);
		return result.first->second;

	} catch (const std::bad_alloc& except) {
		std::cerr << "failed to emplace" << except.what() << std::endl;
		return nullptr;
	}
}
