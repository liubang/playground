//=====================================================================
//
// parser.h -
//
// Created by liubang on 2023/11/02 16:49
// Last Modified: 2023/11/02 16:49
//
//=====================================================================
#include "scanner.h"
#include "token.h"

#include <memory>

namespace pl {

class Parser {
public:
    Parser(const std::string& input)
        : scanner_(std::make_unique<Scanner>(input.data(), input.size())),
          source_(input),
          fname_(""),
          depth_(0) {}

private:
    std::unique_ptr<Scanner> scanner_;
    std::vector<std::string> errs_;
    std::map<TokenType, int32_t> blocks_;
    std::string source_;
    std::string fname_;
    uint32_t depth_;
};

} // namespace pl
