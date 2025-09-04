#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <array>
#include <memory>
#include <unordered_map>
#include <functional>

struct request_t;

using Handler = std::function<int(request_t&)>;

enum METHOD : uint8_t {
	GET,
	POST,
	METHOD_MAX,
};

struct Resolved {
	const Handler* handler;
	std::string param; // uri 상 파라미터 값
};

class RouterNode {
	public:
		std::unordered_map<std::string, std::unique_ptr<RouterNode>> static_children;
		std::unique_ptr<RouterNode> param_child; // uri 경로상 static node 아래에 여러 파라미터 노드는 존재할 수 없기 때문에 1개만 설정 (ex: /users/{user} 와 /users/{id}는 공존할 수 없음,둘 중 하나만 존재 가능)
		std::string param_child_name;

		std::array<Handler, METHOD_MAX> handlers;
};

class Router {
	private:
		RouterNode root_node;
		void split_path_segments(std::vector<std::string>& segs, const std::string& uri) const;
		bool insert_recursive(const METHOD method, std::vector<std::string>::const_iterator begin, std::vector<std::string>::const_iterator end, RouterNode* current_node, const Handler& handler);
		const Handler* find_handler_recursive(const METHOD method, std::vector<std::string>::const_iterator begin, std::vector<std::string>::const_iterator end,
				const RouterNode* current_node, std::string& out_param) const;
		void traverse_print(std::vector<std::string>& segs, const RouterNode* current_node) const;

	public:
		bool add(const METHOD method, const std::string& uri, Handler handler);
		const Resolved resolve(const METHOD method, const std::string& uri) const;
		friend std::ostream& operator<<(std::ostream& os, const Router& router);
};
