#pragma once

#include <iostream>
#include <memory>
#include <unordered_map>

#include <sys/mman.h>   // mmap, munmap
#include <sys/stat.h>   // fstat
#include <fcntl.h>      // open
#include <unistd.h>     // close

class MappedFile {
    public:
        void* data;
        size_t data_len;
    public:
        MappedFile(const char* path) : data(nullptr), data_len(0)
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

        ~MappedFile()
        {
            if (data) {
                if (munmap(data, data_len) == -1) {
                    perror("munmap");
                }
            //  std::cout << __func__ << std::endl;
            }
        }

        const char*  get_data() const
        {
            return static_cast<char*>(data);
        }

        const size_t get_data_len() const
        {
            return data_len;
        }

        friend std::ostream& operator<<(std::ostream& os, const MappedFile& map_file)
        {
            char* data = static_cast<char*>(map_file.data);
            os.write(data, map_file.data_len);

            return os;
        }
};

