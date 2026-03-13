#include "multipart_parser.h"


MultipartFormParser::MultipartFormParser() : count(0)
{
}

MultipartFormParser::~MultipartFormParser()
{
	rename_temporary_filenames();
}

std::string MultipartFormParser::generate_temporary_filename()
{
	std::ostringstream oss;

	oss <<  "h2_tmp_" << this << "_" << std::this_thread::get_id() << "_" << count++;
	return oss.str();
}

void MultipartFormParser::rename_temporary_filenames()
{
	for (auto& pair : file_map) {
		rename(pair.second.c_str(), pair.first.c_str());
	}
}

std::string_view MultipartFormParser::get_filename() const
{
	return current_filename;
}

bool MultipartFormParser::extract_boundary(std::string_view content_type)
{
	if (boundary.empty() && !content_type.empty()) {
		auto start = content_type.find("boundary=");
		if (start != std::string::npos) {
			start += 9;
			auto end = content_type.find("\r\n", start);
			if (end != std::string::npos) {
				boundary = content_type.substr(start, end - start);
				return true;
			} else {
				auto end = content_type.find(";", start);
				if (end != std::string::npos) {
					boundary = content_type.substr(start, end - start);
					return true;
				}

				boundary = content_type.substr(start, content_type.length() - start);
				return true;
			}
		}
	}

	return false;
}

// 다음과 같은 mime 데이터 파싱
//--------------------------c02c4eddc334baf0
//Content-Disposition: form-data; name="upload_file"; filename="test.txt"
//Content-Type: text/plain
//
//hello, hi
//my name is hyeonsu choi ^^
//
//--------------------------c02c4eddc334baf0--
std::pair<size_t, size_t> MultipartFormParser::find_filename(std::string_view buffer, size_t& pos)
{
	pos = buffer.find(boundary + "\r\n");
	if (pos != std::string::npos) {
		// find filename
		std::string filename;
		auto start = buffer.find("filename=\"", pos);
		if (start != std::string::npos) {
			start += 10;
			auto end = buffer.find('"', start);
			if (end != std::string::npos) {
				pos = end;
				return {start, end - start};
			}
		}
	}

	return {std::string::npos, 0};
}

std::pair<size_t, size_t> MultipartFormParser::find_file_content(std::string_view buffer, size_t& pos)
{
	auto start = buffer.find("\r\n\r\n", pos);
	if (start != std::string::npos) {
		start += 4;
		auto end = buffer.find("\r\n", start); // end boundary
		if (end != std::string::npos) {
			pos = end;
			return {start, end - start};
		}
    }

	return {std::string::npos, 0};
}

std::pair<size_t, size_t> MultipartFormParser::find_end_boundary(std::string_view buffer, size_t& pos)
{
	auto start = buffer.find(boundary + "--", pos);
	if (start != std::string::npos) {
		return {start, buffer.length() - start};
	}

	return {std::string::npos, 0};
}

bool MultipartFormParser::write(const std::vector<uint8_t>& buffer)
{
	if (!boundary.empty()) {
		std::string temp_filename;
		size_t pos = 0;
		std::string_view string_buffer(reinterpret_cast<const char*>(buffer.data()), buffer.size());

		auto result = find_filename(string_buffer, pos);
		if (result.first != std::string::npos) {
			auto filename_view = string_buffer.substr(result.first, result.second);
			auto filename = std::string(filename_view);
			auto iter = file_map.find(filename);
			if (iter == file_map.end()) {
				temp_filename = generate_temporary_filename();
				auto result = file_map.emplace(filename, temp_filename);
				if (!result.second) {
					return false;
				}
				current_filename = filename;
				current_tmp_filename = temp_filename; // 임시 파일 이름 사용
				//std::cout << "filename: " << filename << std::endl;
			}
		}

		result = find_file_content(string_buffer, pos);
		if (result.first != std::string::npos) {
			if (!current_tmp_filename.empty()) {
				//std::cout << "contents: " << string_buffer.substr(result.first, result.second) << std::endl;
				std::ofstream out(current_tmp_filename, std::ios::binary | std::ios::app);
				if (out.is_open()) {
					out.write(string_buffer.data() + result.first, result.second);
					out.close();
				}
			}
		}

		result = find_end_boundary(string_buffer, pos);
		if (result.first != std::string::npos) {
			//std::cout << "string buffer: " << string_buffer.substr(result.first, result.second) << std::endl;
			file_map.erase(current_filename);
			rename(current_tmp_filename.c_str(), current_filename.c_str());
			return true;
		}
	}

	return false;
}
