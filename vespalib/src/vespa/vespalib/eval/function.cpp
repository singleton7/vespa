// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/fastos/fastos.h>
#include <cctype>
#include <map>
#include "function.h"
#include "basic_nodes.h"
#include "tensor_nodes.h"
#include "operator_nodes.h"
#include "call_nodes.h"
#include "delete_node.h"

namespace vespalib {
namespace eval {

using nodes::Node_UP;
using nodes::Operator_UP;
using nodes::Call_UP;

namespace {

//-----------------------------------------------------------------------------

class Params {
private:
    std::map<vespalib::string,size_t> _params;
protected:
    size_t lookup(vespalib::stringref token) const {
        auto result = _params.find(token);
        return (result == _params.end()) ? UNDEF : result->second;
    }
    size_t lookup_add(vespalib::stringref token) {
        size_t result = lookup(token);
        if (result == UNDEF) {
            result = _params.size();
            _params[token] = result;
        }
        return result;
    }
public:
    static const size_t UNDEF = -1;
    virtual bool implicit() const = 0;
    virtual size_t resolve(vespalib::stringref token) const = 0;
    std::vector<vespalib::string> extract() const {
        std::vector<vespalib::string> params_out;
        params_out.resize(_params.size());
        for (const auto &item: _params) {
            params_out[item.second] = item.first;
        }
        return params_out;
    }
    virtual ~Params() {}
};

struct ExplicitParams : Params {
    explicit ExplicitParams(const std::vector<vespalib::string> &params_in) {
        for (const auto &param: params_in) {
            assert(lookup(param) == UNDEF);
            lookup_add(param);
        }
    }
    virtual bool implicit() const { return false; }
    virtual size_t resolve(vespalib::stringref token) const override {
        return lookup(token);
    }
};

struct ImplicitParams : Params {
    virtual bool implicit() const { return true; }
    virtual size_t resolve(vespalib::stringref token) const override {
        return const_cast<ImplicitParams*>(this)->lookup_add(token);
    }
};

//-----------------------------------------------------------------------------

class ResolveContext
{
private:
    const Params                  &_params;
    const SymbolExtractor         *_symbol_extractor;
    std::vector<vespalib::string>  _let_names;
public:
    ResolveContext(const Params &params, const SymbolExtractor *symbol_extractor)
        : _params(params), _symbol_extractor(symbol_extractor), _let_names() {}

    void push_let_name(const vespalib::string &name) {
        _let_names.push_back(name);
    }

    void pop_let_name() {
        assert(!_let_names.empty());
        _let_names.pop_back();
    }

    int resolve_let_name(const vespalib::string &name) const {
        for (int i = (int(_let_names.size()) - 1); i >= 0; --i) {
            if (name == _let_names[i]) {
                return -(i + 1);
            }
        }
        return nodes::Symbol::UNDEF;
    }

    int resolve_param(const vespalib::string &name) const {
        size_t param_id = _params.resolve(name);
        if (param_id == Params::UNDEF) {
            return nodes::Symbol::UNDEF;
        }
        return param_id;
    }

    const SymbolExtractor *symbol_extractor() const { return _symbol_extractor; }
};

class ParseContext
{
private:
    const char                  *_begin;
    const char                  *_pos;
    const char                  *_end;
    char                         _curr;
    vespalib::string             _scratch;
    vespalib::string             _failure;
    std::vector<Node_UP>         _expression_stack;
    std::vector<Operator_UP>     _operator_stack;
    size_t                       _operator_mark;
    std::vector<ResolveContext>  _resolve_stack;

public:
    ParseContext(const Params &params, const char *str, size_t len,
                 const SymbolExtractor *symbol_extractor)
        : _begin(str), _pos(str), _end(str + len), _curr(0),
          _scratch(), _failure(),
          _expression_stack(), _operator_stack(),
          _operator_mark(0),
          _resolve_stack({ResolveContext(params, symbol_extractor)})
    {
        if (_pos < _end) {
            _curr = *_pos;
        }
    }
    ~ParseContext() {
        for (size_t i = 0; i < _expression_stack.size(); ++i) {
            delete_node(std::move(_expression_stack[i]));
        }
        _expression_stack.clear();
    }

    ResolveContext &resolver() {
        assert(!_resolve_stack.empty());
        return _resolve_stack.back();
    }

    const ResolveContext &resolver() const {
        assert(!_resolve_stack.empty());
        return _resolve_stack.back();
    }

    void push_resolve_context(const Params &params, const SymbolExtractor *symbol_extractor) {
        _resolve_stack.emplace_back(params, symbol_extractor);
    }

    void pop_resolve_context() {
        assert(!_resolve_stack.empty());
        _resolve_stack.pop_back();
    }

    void fail(const vespalib::string &msg) {
        if (_failure.empty()) {
            _failure = msg;
            _curr = 0;
        }
    }
    bool failed() const { return !_failure.empty(); }
    void next() { _curr = (_curr && (_pos < _end)) ? *(++_pos) : 0; }

    struct InputMark {
        const char *pos;
        char curr;
    };

    InputMark get_input_mark() const { return InputMark{_pos, _curr}; }
    void restore_input_mark(InputMark mark) {
        if ((_curr == 0) && (mark.curr != 0)) {
            _failure.clear();
        }
        _pos = mark.pos;
        _curr = mark.curr;
    }

    char get() const { return _curr; }
    bool eos() const { return !_curr; }
    void eat(char c) {
        if (_curr == c) {
            next();
        } else {
            fail(make_string("expected '%c', but got '%c'", c, _curr));
        }
    }
    void skip_spaces() {
        while (!eos() && isspace(_curr)) {
            next();
        }
    }
    vespalib::string &scratch() {
        _scratch.clear();
        return _scratch;
    }
    vespalib::string &peek(vespalib::string &str, size_t n) {
        const char *p = _pos;
        for (size_t i = 0; i < n; ++i, ++p) {
            if (_curr != 0 && p < _end) {
                str.push_back(*p);
            } else {
                str.push_back(0);
            }
        }
        return str;
    }
    void skip(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            next();
        }
    }

    void push_let_binding(const vespalib::string &name) {
        resolver().push_let_name(name);
    }

    void pop_let_binding() {
        resolver().pop_let_name();
    }

    int resolve_let_ref(const vespalib::string &name) const {
        return resolver().resolve_let_name(name);
    }

    int resolve_parameter(const vespalib::string &name) const {
        return resolver().resolve_param(name);
    }

    void extract_symbol(vespalib::string &symbol_out, InputMark before_symbol) {
        const SymbolExtractor *symbol_extractor = resolver().symbol_extractor();
        if (symbol_extractor == nullptr) {
            return;
        }
        symbol_out.clear();
        restore_input_mark(before_symbol);
        if (!eos()) {
            const char *new_pos = nullptr;
            symbol_extractor->extract_symbol(_pos, _end, new_pos, symbol_out);
            if ((new_pos != nullptr) && (new_pos > _pos) && (new_pos <= _end)) {
                _pos = new_pos;
                _curr = (_pos < _end) ? *_pos : 0;
            } else {
                symbol_out.clear();
            }
        }
    }

    Node_UP get_result() {
        if (!eos() || (num_expressions() != 1) || (num_operators() > 0)) {
            fail("incomplete parse");
        }
        if (!_failure.empty()) {
            vespalib::string before(_begin, (_pos - _begin));
            vespalib::string after(_pos, (_end - _pos));
            return Node_UP(new nodes::Error(make_string("[%s]...[%s]...[%s]",
                                    before.c_str(), _failure.c_str(), after.c_str())));
        }
        return pop_expression();
    }

    void apply_operator() {
        Operator_UP op = pop_operator();
        Node_UP rhs = pop_expression();
        Node_UP lhs = pop_expression();
        op->bind(std::move(lhs), std::move(rhs));
        push_expression(std::move(op));
    }
    size_t num_expressions() const { return _expression_stack.size(); }
    void push_expression(Node_UP node) {
        _expression_stack.push_back(std::move(node));
    }
    Node_UP pop_expression() {
        if (_expression_stack.empty()) {
            fail("expression stack underflow");
            return Node_UP(new nodes::Number(0.0));
        }
        Node_UP node = std::move(_expression_stack.back());
        _expression_stack.pop_back();
        return node;
    }
    size_t num_operators() const { return _operator_stack.size(); }

    size_t operator_mark() const { return _operator_mark; }
    void operator_mark(size_t mark) { _operator_mark = mark; }

    void push_operator(Operator_UP node) {
        while ((_operator_stack.size() > _operator_mark) &&
               (_operator_stack.back()->do_before(*node)))
        {
            apply_operator();
        }
        _operator_stack.push_back(std::move(node));
    }
    Operator_UP pop_operator() {
        assert(!_operator_stack.empty());
        Operator_UP node = std::move(_operator_stack.back());
        _operator_stack.pop_back();
        return node;
    }
};

//-----------------------------------------------------------------------------

void parse_expression(ParseContext &ctx);

int unhex(char c) {
    if (c >= '0' && c <= '9') {
        return (c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return ((c - 'a') + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return ((c - 'A') + 10);
    }
    return -1;
}

void parse_string(ParseContext &ctx) {
    vespalib::string &str = ctx.scratch();
    ctx.eat('"');
    while (!ctx.eos() && ctx.get() != '"') {
        if (ctx.get() == '\\') {
            ctx.next();
            if (ctx.get() == 'x') {
                ctx.next();
                int hex1 = unhex(ctx.get());
                ctx.next();
                int hex2 = unhex(ctx.get());
                if (hex1 < 0 || hex2 < 0) {
                    ctx.fail("bad hex quote");
                }
                str.push_back((hex1 << 4) + hex2);
            } else {
                switch(ctx.get()) {
                case '"':  str.push_back('"');  break;
                case '\\': str.push_back('\\'); break;
                case 'f':  str.push_back('\f'); break;
                case 'n':  str.push_back('\n'); break;
                case 'r':  str.push_back('\r'); break;
                case 't':  str.push_back('\t'); break;
                default: ctx.fail("bad quote"); break;
                }
            }
        } else {
            str.push_back(ctx.get()); // default case
        }
        ctx.next();
    }
    ctx.eat('"');
    ctx.push_expression(Node_UP(new nodes::String(str)));
}

void parse_number(ParseContext &ctx) {
    vespalib::string &str = ctx.scratch();
    str.push_back(ctx.get());
    ctx.next();
    while (ctx.get() >= '0' && ctx.get() <= '9') {
        str.push_back(ctx.get());
        ctx.next();
    }
    if (ctx.get() == '.') {
        str.push_back(ctx.get());
        ctx.next();
        while (ctx.get() >= '0' && ctx.get() <= '9') {
            str.push_back(ctx.get());
            ctx.next();
        }
    }
    if (ctx.get() == 'e' || ctx.get() == 'E') {
        str.push_back(ctx.get());
        ctx.next();
        if (ctx.get() == '+' || ctx.get() == '-') {
            str.push_back(ctx.get());
            ctx.next();
        }
        while (ctx.get() >= '0' && ctx.get() <= '9') {
            str.push_back(ctx.get());
            ctx.next();
        }
    }
    char *end = nullptr;
    double value = strtod(str.c_str(), &end);
    if (!str.empty() && end == str.data() + str.size()) {
        ctx.push_expression(Node_UP(new nodes::Number(value)));
    } else {
        ctx.fail(make_string("invalid number: '%s'", str.c_str()));
    }
    return;
}

// NOTE: using non-standard definition of identifiers
// (to match ranking expression parser in Java)
bool is_ident(char c, bool first) {
    return ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            (c == '_') || (c == '@') ||
            (c == '$' && !first));
}

vespalib::string get_ident(ParseContext &ctx) {
    ctx.skip_spaces();
    vespalib::string ident;
    if (is_ident(ctx.get(), true)) {
        ident.push_back(ctx.get());
        for (ctx.next(); is_ident(ctx.get(), false); ctx.next()) {
            ident.push_back(ctx.get());
        }
    }
    return ident;
}

void parse_if(ParseContext &ctx) {
    parse_expression(ctx);
    Node_UP cond = ctx.pop_expression();
    ctx.eat(',');
    parse_expression(ctx);
    Node_UP true_expr = ctx.pop_expression();
    ctx.eat(',');
    parse_expression(ctx);
    Node_UP false_expr = ctx.pop_expression();
    double p_true = 0.5;
    if (ctx.get() == ',') {
        ctx.eat(',');
        parse_number(ctx);
        Node_UP p_true_node = ctx.pop_expression();
        auto p_true_number = nodes::as<nodes::Number>(*p_true_node);
        if (p_true_number) {
            p_true = p_true_number->value();
        }
    }
    ctx.push_expression(Node_UP(new nodes::If(std::move(cond), std::move(true_expr), std::move(false_expr), p_true)));
}

void parse_let(ParseContext &ctx) {
    vespalib::string name = get_ident(ctx);
    ctx.skip_spaces();
    ctx.eat(',');
    parse_expression(ctx);
    Node_UP value = ctx.pop_expression();
    ctx.eat(',');
    ctx.push_let_binding(name);
    parse_expression(ctx);
    Node_UP expr = ctx.pop_expression();
    ctx.pop_let_binding();
    ctx.push_expression(Node_UP(new nodes::Let(name, std::move(value), std::move(expr))));
}

void parse_call(ParseContext &ctx, Call_UP call) {
    for (size_t i = 0; i < call->num_params(); ++i) {
        if (i > 0) {
            ctx.eat(',');
        }
        parse_expression(ctx);
        call->bind_next(ctx.pop_expression());
    }
    ctx.push_expression(std::move(call));
}

// (a,b,c)
std::vector<vespalib::string> get_ident_list(ParseContext &ctx) {
    std::vector<vespalib::string> list;
    ctx.skip_spaces();
    ctx.eat('(');
    for (ctx.skip_spaces(); !ctx.eos() && (ctx.get() != ')'); ctx.skip_spaces()) {
        if (!list.empty()) {
            ctx.eat(',');
        }
        list.push_back(get_ident(ctx));
    }
    ctx.eat(')');
    return list;
}

Function parse_lambda(ParseContext &ctx) {
    ctx.skip_spaces();
    ctx.eat('f');
    auto param_names = get_ident_list(ctx);
    ExplicitParams params(param_names);
    ctx.push_resolve_context(params, nullptr);
    ctx.skip_spaces();
    ctx.eat('(');
    parse_expression(ctx);
    ctx.eat(')');
    ctx.pop_resolve_context();
    Node_UP lambda_root = ctx.pop_expression();
    return Function(std::move(lambda_root), std::move(param_names));
}

void parse_tensor_map(ParseContext &ctx) {
    parse_expression(ctx);
    Node_UP child = ctx.pop_expression();
    ctx.eat(',');
    Function lambda = parse_lambda(ctx);
    if (lambda.num_params() != 1) {
        ctx.fail(make_string("map requires a lambda with 1 parameter, was %zu",
                             lambda.num_params()));
    }
}

void parse_tensor_join(ParseContext &ctx) {
    parse_expression(ctx);
    Node_UP lhs = ctx.pop_expression();
    ctx.eat(',');
    parse_expression(ctx);
    Node_UP rhs = ctx.pop_expression();
    ctx.eat(',');
    Function lambda = parse_lambda(ctx);
    if (lambda.num_params() != 2) {
        ctx.fail(make_string("join requires a lambda with 2 parameter, was %zu",
                             lambda.num_params()));
    }
}

// to be replaced with more generic 'reduce'
void parse_tensor_sum(ParseContext &ctx) {
    parse_expression(ctx);
    Node_UP child = ctx.pop_expression();
    if (ctx.get() == ',') {
        ctx.next();
        vespalib::string dimension = get_ident(ctx);
        ctx.skip_spaces();
        ctx.push_expression(Node_UP(new nodes::TensorSum(std::move(child), dimension)));
    } else {
        ctx.push_expression(Node_UP(new nodes::TensorSum(std::move(child))));
    }
}

bool try_parse_call(ParseContext &ctx, const vespalib::string &name) {
    ctx.skip_spaces();
    if (ctx.get() == '(') {
        ctx.eat('(');
        if (name == "if") {
            parse_if(ctx);
        } else if (name == "let") {
            parse_let(ctx);
        } else {
            Call_UP call = nodes::CallRepo::instance().create(name);
            if (call.get() != nullptr) {
                parse_call(ctx, std::move(call));
            } else if (name == "map") {
                parse_tensor_map(ctx);
            } else if (name == "join") {
                parse_tensor_join(ctx);
            } else if (name == "sum") {
                parse_tensor_sum(ctx);
            } else {
                ctx.fail(make_string("unknown function: '%s'", name.c_str()));
                return false;
            }
        }
        ctx.eat(')');
        return true;
    }
    return false;
}

int parse_symbol(ParseContext &ctx, vespalib::string &name, ParseContext::InputMark before_name) {
    int id = ctx.resolve_let_ref(name);
    if (id != nodes::Symbol::UNDEF) {
        return id;
    }
    ctx.extract_symbol(name, before_name);
    return ctx.resolve_parameter(name);
}

void parse_symbol_or_call(ParseContext &ctx) {
    ParseContext::InputMark before_name = ctx.get_input_mark();
    vespalib::string name = get_ident(ctx);
    if (!try_parse_call(ctx, name)) {
        int id = parse_symbol(ctx, name, before_name);
        if (name.empty()) {
            ctx.fail("missing value");
        } else if (id == nodes::Symbol::UNDEF) {
            ctx.fail(make_string("unknown symbol: '%s'", name.c_str()));
        } else {
            ctx.push_expression(Node_UP(new nodes::Symbol(id)));
        }
    }
}

void parse_array(ParseContext &ctx) {
    std::unique_ptr<nodes::Array> array(new nodes::Array());
    ctx.eat('[');
    ctx.skip_spaces();
    size_t size = 0;
    while (!ctx.eos() && ctx.get() != ']') {
        if (++size > 1) {
            ctx.eat(',');
        }
        parse_expression(ctx);
        array->add(ctx.pop_expression());
    }
    ctx.eat(']');
    ctx.push_expression(std::move(array));
}

void parse_value(ParseContext &ctx) {
    ctx.skip_spaces();
    if (ctx.get() == '-') {
        ctx.next();
        parse_value(ctx);
        ctx.push_expression(Node_UP(new nodes::Neg(ctx.pop_expression())));
    } else if (ctx.get() == '!') {
        ctx.next();
        parse_value(ctx);
        ctx.push_expression(Node_UP(new nodes::Not(ctx.pop_expression())));
    } else if (ctx.get() == '(') {
        ctx.next();
        parse_expression(ctx);
        ctx.eat(')');
    } else if (ctx.get() == '[') {
        parse_array(ctx);
    } else if (ctx.get() == '"') {
        parse_string(ctx);
    } else if (isdigit(ctx.get())) {
        parse_number(ctx);
    } else {
        parse_symbol_or_call(ctx);
    }
}

void parse_operator(ParseContext &ctx) {
    ctx.skip_spaces();
    vespalib::string &str = ctx.peek(ctx.scratch(), nodes::OperatorRepo::instance().max_size());
    Operator_UP op = nodes::OperatorRepo::instance().create(str);
    if (op.get() != nullptr) {
        ctx.push_operator(std::move(op));
        ctx.skip(str.size());
    } else {
        ctx.fail(make_string("invalid operator: '%c'", ctx.get()));
    }
}

void parse_expression(ParseContext &ctx) {
    size_t old_mark = ctx.operator_mark();
    ctx.operator_mark(ctx.num_operators());
    for (;;) {
        parse_value(ctx);
        ctx.skip_spaces();
        if (ctx.eos() || ctx.get() == ')' || ctx.get() == ',' || ctx.get() == ']') {
            while (ctx.num_operators() > ctx.operator_mark()) {
                ctx.apply_operator();
            }
            ctx.operator_mark(old_mark);
            return;
        }
        parse_operator(ctx);
    }
}

Function parse_function(const Params &params, vespalib::stringref expression,
                        const SymbolExtractor *symbol_extractor)
{
    ParseContext ctx(params, expression.data(), expression.size(), symbol_extractor);
    parse_expression(ctx);
    if (ctx.failed() && params.implicit()) {
        return Function(ctx.get_result(), std::vector<vespalib::string>());
    }
    return Function(ctx.get_result(), params.extract());
}

} // namespace vespalib::<unnamed>

//-----------------------------------------------------------------------------

bool
Function::has_error() const
{
    auto error = nodes::as<nodes::Error>(*_root);
    return error;
}

vespalib::string
Function::get_error() const
{
    auto error = nodes::as<nodes::Error>(*_root);
    return error ? error->message() : "";
}

Function
Function::parse(vespalib::stringref expression)
{
    return parse_function(ImplicitParams(), expression, nullptr);
}

Function
Function::parse(vespalib::stringref expression, const SymbolExtractor &symbol_extractor)
{
    return parse_function(ImplicitParams(), expression, &symbol_extractor);
}

Function
Function::parse(const std::vector<vespalib::string> &params, vespalib::stringref expression)
{
    return parse_function(ExplicitParams(params), expression, nullptr);
}

Function
Function::parse(const std::vector<vespalib::string> &params, vespalib::stringref expression,
                const SymbolExtractor &symbol_extractor)
{
    return parse_function(ExplicitParams(params), expression, &symbol_extractor);
}

//-----------------------------------------------------------------------------

bool
Function::unwrap(vespalib::stringref input,
                 vespalib::string &wrapper,
                 vespalib::string &body,
                 vespalib::string &error)
{
    size_t pos = 0;
    for (; pos < input.size() && isspace(input[pos]); ++pos);
    size_t wrapper_begin = pos;
    for (; pos < input.size() && isalpha(input[pos]); ++pos);
    size_t wrapper_end = pos;
    if (wrapper_end == wrapper_begin) {
        error = "could not extract wrapper name";
        return false;
    }
    for (; pos < input.size() && isspace(input[pos]); ++pos);
    if (pos == input.size() || input[pos] != '(') {
        error = "could not match opening '('";
        return false;
    }
    size_t body_begin = (pos + 1);
    size_t body_end = (input.size() - 1);
    for (; body_end > body_begin && isspace(input[body_end]); --body_end);
    if (input[body_end] != ')') {
        error = "could not match closing ')'";
        return false;
    }
    assert(body_end >= body_begin);
    wrapper = vespalib::stringref(input.data() + wrapper_begin, wrapper_end - wrapper_begin);
    body = vespalib::stringref(input.data() + body_begin, body_end - body_begin);
    return true;
}

//-----------------------------------------------------------------------------

} // namespace vespalib::eval
} // namespace vespalib
