// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include <string>
#include <vector>
#include "cpp/pl/influxql/token.h"

%%{
    machine influxql;

    alphtype unsigned char;

    include WChar "unicode.rl";

    action advance_line {
        // We do this for every newline we find.
        // This allows us to return correct line/column for each token
        // back to the caller.
        cur_line += 1;
        *last_newline = fpc + 1;
    }

    action advance_line_between_tokens {
        // We do this for each newline we find in the whitespace between tokens,
        // so we can record the location of the first byte of a token.
        last_newline_before_token = *last_newline;
        cur_line_token_start = cur_line;
    }

    newline = '\n' @advance_line;
    any_count_line = any | newline;

    identifier = ( ualpha | "_" ) ( ualnum | "_" )*;

    int_lit = digit+;

    float_lit = (digit+ "." digit*) | ("." digit+);

    duration_unit = "y" | "mo" | "w" | "d" | "h" | "m" | "s" | "ms" | "us" | "ns";
    duration_lit = ( int_lit duration_unit )+;

    date = digit{4} "-" digit{2} "-" digit{2};
    time_offset = "Z" | (("+" | "-") digit{2} ":" digit{2});
    time = digit{2} ":" digit{2} ":" digit{2} ( "." digit* )? time_offset?;
    date_time_lit = date ( "T" time )?;

    escaped_char = "\\" ( "n" | "r" | "t" | "\\" | '"' | "${" );
    unicode_value = (any_count_line - [\\$]) | escaped_char;
    byte_value = "\\x" xdigit{2};
    dollar_value = "$" ( any_count_line - [{"] );
    string_lit_char = ( unicode_value | byte_value | dollar_value );
    string_lit = '"' string_lit_char* "$"? :> '"';

    regex_escaped_char = "\\/" ;
    regex_unicode_value = (any_count_line - "/") | regex_escaped_char;
    regex_lit = "/" ( regex_unicode_value | byte_value )+ "/";

    # The newline is optional so that a comment at the end of a file is considered valid.
    single_line_comment = "--" [^\n]* newline?;

    # Whitespace is standard ws and control codes->
    # (Note that newlines are handled separately; see notes above)
    whitespace = (space - '\n')+;

    # The regex literal is not compatible with division so we need two machines->
    # One machine contains the full grammar and is the main one, the other is used to scan when we are
    # in the middle of an expression and we are potentially expecting a division operator.
    main_with_regex := |*
        # If we see a regex literal, we accept that and do not go to the other scanner.
        regex_lit => { tok = TokenType::REGEX; fbreak; };

        # We have to specify whitespace here so that leading whitespace doesn't cause a state transition.
        whitespace;

        newline => advance_line_between_tokens;

        # Any other character we transfer to the main state machine that defines the entire language.
        any => { fhold; fgoto main; };
    *|;

    # This machine does not contain the regex literal.
    main := |*
        single_line_comment => { tok = TokenType::COMMENT; fbreak; };

        /by/i            => { tok = TokenType::BY; fbreak; };
        /queries/i       => { tok = TokenType::QUERIES; fbreak; };
        /revoke/i        => { tok = TokenType::REVOKE; fbreak; };
        /subscriptions/i => { tok = TokenType::SUBSCRIPTIONS; fbreak; };
        /exact/i         => { tok = TokenType::EXACT; fbreak; };
        /field/i         => { tok = TokenType::FIELD; fbreak; };
        /order/i         => { tok = TokenType::ORDER; fbreak; };
        /policies/i      => { tok = TokenType::POLICIES; fbreak; };
        /set/i           => { tok = TokenType::SET; fbreak; };
        /slimit/i        => { tok = TokenType::SLIMIT; fbreak; };
        /any/i           => { tok = TokenType::ANY; fbreak; };
        /asc/i           => { tok = TokenType::ASC; fbreak; };
        /database/i      => { tok = TokenType::DATABASE; fbreak; };
        /keys/i          => { tok = TokenType::KEYS; fbreak; };
        /measurements/i  => { tok = TokenType::MEASUREMENTS; fbreak; };
        /select/i        => { tok = TokenType::SELECT; fbreak; };
        /shard/i         => { tok = TokenType::SHARD; fbreak; };
        /to/i            => { tok = TokenType::TO; fbreak; };
        /with/i          => { tok = TokenType::WITH; fbreak; };
        /continuous/i    => { tok = TokenType::CONTINUOUS; fbreak; };
        /duration/i      => { tok = TokenType::DURATION; fbreak; };
        /insert/i        => { tok = TokenType::INSERT; fbreak; };
        /user/i          => { tok = TokenType::USER; fbreak; };
        /where/i         => { tok = TokenType::WHERE; fbreak; };
        /desc/i          => { tok = TokenType::DESC; fbreak; };
        /destinations/i  => { tok = TokenType::DESTINATIONS; fbreak; };
        /diagnostics/i   => { tok = TokenType::DIAGNOSTICS; fbreak; };
        /for/i           => { tok = TokenType::FOR; fbreak; };
        /key/i           => { tok = TokenType::KEY; fbreak; };
        /limit/i         => { tok = TokenType::LIMIT; fbreak; };
        /password/i      => { tok = TokenType::PASSWORD; fbreak; };
        /values/i        => { tok = TokenType::VALUES; fbreak; };
        /from/i          => { tok = TokenType::FROM; fbreak; };
        /false/i         => { tok = TokenType::FALSE; fbreak; };
        /distinct/i      => { tok = TokenType::DISTINCT; fbreak; };
        /every/i         => { tok = TokenType::EVERY; fbreak; };
        /policy/i        => { tok = TokenType::POLICY; fbreak; };
        /or/i            => { tok = TokenType::OR; fbreak; };
        /all/i           => { tok = TokenType::ALL; fbreak; };
        /cardinality/i   => { tok = TokenType::CARDINALITY; fbreak; };
        /delete/i        => { tok = TokenType::DELETE; fbreak; };
        /kill/i          => { tok = TokenType::KILL; fbreak; };
        /inf/i           => { tok = TokenType::INF; fbreak; };
        /query/i         => { tok = TokenType::QUERY; fbreak; };
        /series/i        => { tok = TokenType::SERIES; fbreak; };
        /drop/i          => { tok = TokenType::DROP; fbreak; };
        /end/i           => { tok = TokenType::END; fbreak; };
        /offset/i        => { tok = TokenType::OFFSET; fbreak; };
        /retention/i     => { tok = TokenType::RETENTION; fbreak; };
        /explain/i       => { tok = TokenType::EXPLAIN; fbreak; };
        /grants/i        => { tok = TokenType::GRANTS; fbreak; };
        /name/i          => { tok = TokenType::NAME; fbreak; };
        /read/i          => { tok = TokenType::READ; fbreak; };
        /and/i           => { tok = TokenType::AND; fbreak; };
        /analyze/i       => { tok = TokenType::ANALYZE; fbreak; };
        /begin/i         => { tok = TokenType::BEGIN; fbreak; };
        /databases/i     => { tok = TokenType::DATABASES; fbreak; };
        /write/i         => { tok = TokenType::WRITE; fbreak; };
        /create/i        => { tok = TokenType::CREATE; fbreak; };
        /in/i            => { tok = TokenType::IN; fbreak; };
        /privileges/i    => { tok = TokenType::PRIVILEGES; fbreak; };
        /replication/i   => { tok = TokenType::REPLICATION; fbreak; };
        /resample/i      => { tok = TokenType::RESAMPLE; fbreak; };
        /show/i          => { tok = TokenType::SHOW; fbreak; };
        /stats/i         => { tok = TokenType::STATS; fbreak; };
        /subscription/i  => { tok = TokenType::SUBSCRIPTION; fbreak; };
        /group/i         => { tok = TokenType::GROUP; fbreak; };
        /into/i          => { tok = TokenType::INTO; fbreak; };
        /on/i            => { tok = TokenType::ON; fbreak; };
        /users/i         => { tok = TokenType::USERS; fbreak; };
        /alter/i         => { tok = TokenType::ALTER; fbreak; };
        /default/i       => { tok = TokenType::DEFAULT; fbreak; };
        /soffset/i       => { tok = TokenType::SOFFSET; fbreak; };
        /tag/i           => { tok = TokenType::TAG; fbreak; };
        /as/i            => { tok = TokenType::AS; fbreak; };
        /grant/i         => { tok = TokenType::GRANT; fbreak; };
        /group/i         => { tok = TokenType::GROUP; fbreak; };
        /measurement/i   => { tok = TokenType::MEASUREMENT; fbreak; };
        /shards/i        => { tok = TokenType::SHARDS; fbreak; };
        /true/i          => { tok = TokenType::TRUE; fbreak; };
        
        identifier      => { tok = TokenType::IDENT; fbreak; };
        int_lit         => { tok = TokenType::INTEGER; fbreak; };
        float_lit       => { tok = TokenType::NUMBER; fbreak; };
        duration_lit    => { tok = TokenType::DURATION; fbreak; };
        # date_time_lit => { tok = TokenType::Time; fbreak; };
        string_lit      => { tok = TokenType::STRING; fbreak; };

        # operators
        "+"  => { tok = TokenType::ADD; fbreak; };
        "-"  => { tok = TokenType::SUB; fbreak; };
        "*"  => { tok = TokenType::MUL; fbreak; };
        "/"  => { tok = TokenType::DIV; fbreak; };
        "%"  => { tok = TokenType::MOD; fbreak; };
        "&"  => { tok = TokenType::BITWISE_AND; fbreak; };
        "|"  => { tok = TokenType::BITWISE_OR; fbreak; };
        "^"  => { tok = TokenType::BITWISE_XOR; fbreak; };
        "="  => { tok = TokenType::EQ; fbreak; };
        "!=" => { tok = TokenType::NEQ; fbreak; };
        "=~" => { tok = TokenType::EQREGEX; fbreak; };
        "!~" => { tok = TokenType::NEQREGEX; fbreak; };
        "<"  => { tok = TokenType::LT; fbreak; };
        "<=" => { tok = TokenType::LTE; fbreak; };
        ">"  => { tok = TokenType::GT; fbreak; };
        ">=" => { tok = TokenType::GTE; fbreak; };

        "("  => { tok = TokenType::LPAREN; fbreak; };
        ")"  => { tok = TokenType::RPAREN; fbreak; };
        ","  => { tok = TokenType::COMMA; fbreak; };
        ":"  => { tok = TokenType::COLON; fbreak; };
        "::" => { tok = TokenType::DOUBLECOLON; fbreak; };
        ";"  => { tok = TokenType::SEMICOLON; fbreak; };
        "."  => { tok = TokenType::DOT; fbreak; };
        
        whitespace;

        newline => advance_line_between_tokens;
    *|;

}%%

namespace pl {

%% write data nofinal;

uint32_t real_scan(
    int32_t mode,
    const char** pp,
    const char* _data,
    const char* pe,
    const char* eof,
    const char** last_newline,
    int32_t& cur_line,
    TokenType& token,
    int32_t& token_start,
    int32_t& token_start_line,
    int32_t& token_start_col,
    int32_t& token_end,
    int32_t& token_end_line,
    int32_t& token_end_col)
{
    int cs = influxql_start;
    switch (cs) {
    case 0:
        cs = influxql_en_main;
        break;
    case 1:
        cs = influxql_en_main_with_regex;
        break;
    default:
        break;
    }
    
    const char* p = *pp;

    int32_t act = 0;
    const char* ts = 0;
    const char* te = 0;

    TokenType tok = TokenType::ILLEGAL;

    const char* last_newline_before_token = *last_newline;
    uint32_t cur_line_token_start = cur_line;

    // alskdfj
    %% write init nocs;
    %% write exec;

    // Update output args.
    token = tok;

    token_start = ts - _data;
    token_start_line = cur_line_token_start;
    token_start_col = ts - last_newline_before_token + 1;

    token_end = te - _data;

    if (*last_newline > te) {
        // te (the token end pointer) will only be less than last_newline
        // (pointer to the last newline the scanner saw) if we are trying
        // to find a multi-line token (either string or regex literal)
        // but don't find the closing `/` or `"`.
        // In that case we need to reset last_newline and cur_line.
        cur_line = cur_line_token_start;
        *last_newline = last_newline_before_token;
    }

    token_end_line = cur_line;
    token_end_col = te - *last_newline + 1;

    *pp = p;
    if (cs == influxql_error) {
        return 1;
    } else {
        return 0;
    }
}
}
