#pragma once

#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <vector>
#include <utility>
#include <unordered_map>

class MultipartFormParser {
	private:
		std::string current_filename;
		std::string current_tmp_filename; // 현재 파일 이름
		int count; // 파일 넘버링 용도
		std::string boundary;
		std::unordered_map<std::string, std::string> file_map; // key:filename, value: tmp filename
		std::string generate_temporary_filename();

		void rename_temporary_filenames();
		std::pair<size_t, size_t> find_filename(std::string_view buffer, size_t& pos);
		std::pair<size_t, size_t> find_file_content(std::string_view buffer, size_t& pos);
		std::pair<size_t, size_t> find_end_boundary(std::string_view buffer, size_t& pos);

	public:
		MultipartFormParser();
		~MultipartFormParser();
		std::string_view get_filename() const;
		bool extract_boundary(std::string_view content_type);
		bool write(const std::vector<uint8_t>& buffer);
};
