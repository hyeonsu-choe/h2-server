#include "router.h"
#include <iostream>

void Router::split_path_segments(std::vector<std::string_view>& segs, std::string_view uri) const
{
	segs.clear();
	size_t start = 0;
	while ((start = uri.find_first_not_of('/', start)) != std::string_view::npos) {
		size_t end = uri.find('/', start);
		if (end == std::string_view::npos) {
			segs.push_back(uri.substr(start)); // uri의 마지막 seg면 작업 종료
			break;
		}
		segs.push_back(uri.substr(start, end - start)); // 현재 seg 저장
		start = end;
	}
}

bool Router::add(const METHOD method, std::string_view uri, Handler handler)
{
	std::vector<std::string_view> segs;
	split_path_segments(segs, uri);

	return insert_recursive(method, segs.cbegin(), segs.cend(), &root_node, std::move(handler));
}

bool Router::insert_recursive(const METHOD method, std::vector<std::string_view>::const_iterator begin, std::vector<std::string_view>::const_iterator end,
		RouterNode* current_node, const Handler& handler)
{
	if (begin == end) {
		current_node->handlers[method] = handler;
		return true;
	}

	auto static_child_itr = current_node->static_children.find(std::string(*begin));
	if (static_child_itr != current_node->static_children.end()) {
		return insert_recursive(method, begin + 1, end, static_child_itr->second.get(), handler);	
	}

	// 파라미터 처리 
	if (begin->size() >= 2 && begin->front() == '{' && begin->back() == '}') {
		if (begin->length() < 3) {
			return false;
		}

		// 파라미터 노드가 이미 존재하면 중복 할당 시도로 실패 판정
		if (current_node->param_child) {
			return false;
		}
		current_node->param_child = std::make_unique<RouterNode>();
		current_node->param_child_name = std::string(begin->substr(1, begin->length() - 2)); // 실제 문자열 할당이 필요하므로 string 생성
		return insert_recursive(method, begin + 1, end, current_node->param_child.get(), handler);
	} else {
		// static node 할당
		std::string seg(*begin);
		auto result = current_node->static_children.insert({seg, std::make_unique<RouterNode>()});
		if (result.second) {
			return insert_recursive(method, begin + 1, end, result.first->second.get(), handler);
		}

	}

	return false;
}

const Handler* Router::find_handler_recursive(const METHOD method,
												std::vector<std::string_view>::const_iterator begin,
												std::vector<std::string_view>::const_iterator end,
												const RouterNode* current_node,
												std::string_view& out_param) const
{
	if (begin == end) {
		return current_node->handlers[method] ? &current_node->handlers[method] : nullptr;
	}

	// 정적 경로 검색
	auto static_child_itr = current_node->static_children.find(std::string(*begin));
	if (static_child_itr != current_node->static_children.end()) {
		auto handler = find_handler_recursive(method, begin + 1, end, static_child_itr->second.get(), out_param);	
		if (handler) {
			return handler;
		}
	}

	// 파라미터 경로 검색
	if (current_node->param_child) {
		std::string_view prev_param = out_param;
		out_param = *begin;

		auto handler = find_handler_recursive(method, begin + 1, end, current_node->param_child.get(), out_param);
		if (handler) {
			return handler;
		}

		out_param = prev_param;
	}

	return nullptr;
}

const Resolved Router::resolve(const METHOD method, std::string_view uri) const
{
	std::vector<std::string_view> segs;
	split_path_segments(segs, uri);
	std::string_view captured_param;

	auto handler = find_handler_recursive(method, segs.cbegin(), segs.cend(), &root_node, captured_param);
	return Resolved{handler, captured_param};
}

void Router::traverse_print(std::vector<std::string_view>& segs, const RouterNode* current_node) const
{
	for (const auto& iter : current_node->static_children) {
		segs.push_back(iter.first);
		traverse_print(segs, iter.second.get());
		segs.pop_back();
	}

	if (current_node->param_child) {
		std::string param_name = "{" + current_node->param_child_name + "}";
		segs.push_back(param_name);
		traverse_print(segs, current_node->param_child.get());
		segs.pop_back();
	}

	for (uint8_t index = 0; index < current_node->handlers.size(); index++) {
		if (current_node->handlers[index]) {
			if (index == GET) {
				std::cout << "GET ";
			} else {
				std::cout << "POST";
			}

			for (auto& seg : segs) {
				std::cout << "/" << seg;	
			}
			std::cout << std::endl;
		}
	}
}

std::ostream& operator<<(std::ostream& os, const Router& router)
{
	std::vector<std::string_view> segs;
	router.traverse_print(segs, &router.root_node);
	return os;
}
