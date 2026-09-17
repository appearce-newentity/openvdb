// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

////////////////////////////////////////////////////////////////////////////////
///
/// @author Ken Museth
///
/// @file Parser.h
///
/// @brief Command-line argv parser and supporting infrastructure for the
///        action / option / expression / loop language used by vdb_tool.
///        Defines:
///          - Option, Action — declarative description of a registered action
///            (names, option list, init/run lambdas, anonymous-arg index,
///            greedy flag for actions whose value may contain spaces).
///          - Memory, Stack — the string-keyed variable store and the working
///            stack shared with -eval / -calc.
///          - Processor — RPN evaluator for "{...}" expressions embedded in
///            option values (e.g. "{1:2:+}" -> "3", "{$x:path}" -> dirname).
///          - BaseLoop and the derived ForLoop, EachLoop, FilesLoop, IfLoop —
///            control-flow scopes implementing -for, -each, -files, -if;
///            closed by -end and tracked via the openLoops stack.
///          - Parser — top-level orchestrator: registers actions via
///            addAction, parses argv, drives the run loop, and produces
///            "did you mean" suggestions for typo'd action/option names.
///
/// @warning All prints are directed to cerr since cout is used for piping!
///
////////////////////////////////////////////////////////////////////////////////

#ifndef VDB_TOOL_PARSER_HAS_BEEN_INCLUDED
#define VDB_TOOL_PARSER_HAS_BEEN_INCLUDED

#include <iostream>
#include <sstream>
#include <string> // for std::string, std::stof and std::stoi
#include <algorithm> // std::sort
#include <filesystem>
#include <random>
#include <functional>
#include <vector>
#include <map>// for map and multimap
#include <list>
#include <set>
#include <time.h>
#include <initializer_list>
#include <unordered_map>
#include <iterator>// for std::advance
#include <sys/stat.h>
#include <stdio.h>

#include <openvdb/openvdb.h>
#include <openvdb/util/Assert.h>

#include "Util.h"
#include "Calculator.h"// used by the -if action to evaluate infix/RPN test expressions

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

/// @brief String attributes for a single option (i.e. an argument to an action).
/// @details Each option carries a name, its current value (possibly empty),
///          an example string used in help output, and a documentation string.
struct Option {
    /// @brief Append @a v to value, comma-separating if value is already non-empty.
    void append(const std::string &v) {value = value.empty() ? v : value + "," + v;}

    std::string name;          ///< Option name, e.g. "voxel" or "radius".
    std::string value;         ///< Current value as a string (may contain expressions).
    std::string example;       ///< Example value shown in usage/help output.
    std::string documentation; ///< Human-readable description of the option.
};

// ==============================================================================================================

/// @brief Describes a single command-line action (e.g. "-sphere", "-read")
///        with its aliases, options, and init/run callbacks.
/// @details Actions are registered with the Parser via Parser::addAction. During
///          parsing, the init callback is invoked when the action is encountered;
///          during execution, the run callback is invoked to perform the work.
struct Action {
    /// @brief Constructor.
    /// @param _names      List of names/aliases for this action (e.g. {"read", "import", "load", "i"}).
    /// @param _doc        One-line documentation string shown in usage output.
    /// @param _options    Options accepted by this action.
    /// @param _init       Callback invoked during parsing (typically for syntax/state checks).
    /// @param _run        Callback invoked during execution to perform the action.
    /// @param _anonymous  Index of the option to which un-named option values are appended,
    ///                    or size_t(-1) if anonymous values are disallowed.
    /// @param _greedy     When true, any "name=value" token whose prefix is not
    ///                    a recognized option name is also appended (whole) to
    ///                    the anonymous option, rather than throwing
    ///                    "Invalid option". Used when the anonymous option's
    ///                    value may itself contain '=', e.g. the kernel string
    ///                    in -calc or the kernel of forAllValues / forOnValues
    ///                    / forOffValues. Requires _anonymous to be a valid
    ///                    option index.
    Action(std::vector<std::string> &&_names,
           std::string _doc,
           std::vector<Option> &&_options,
           std::function<void()> &&_init,
           std::function<void()> &&_run,
           size_t _anonymous = -1,
           bool   _greedy    = false)
      : names(std::move(_names))
      , documentation(std::move(_doc))
      , anonymous(_anonymous)
      , greedy(_greedy)
      , options(std::move(_options))
      , init(std::move(_init))
      , run(std::move(_run)) {}

    /// @brief Default copy constructor.
    Action(const Action&) = default;

    /// @brief Parse a single "name=value" token and update the matching option.
    /// @param str Token such as "voxel=0.1" or a bare value appended to the anonymous option.
    /// @throw std::invalid_argument if the token cannot be matched to any option.
    void setOption(const std::string &str);

    /// @brief Return option names that are substring-matched by @a name (used
    ///        to suggest "did you mean" corrections for typos like
    ///        `radiuss=` &rarr; `radius=`). Sorted by best match (smallest
    ///        substring offset). Empty when no option matches.
    std::multimap<size_t, std::string> closeOptionMatches(const std::string &name) const;

    /// @brief Print this action and its current options in the canonical "-name opt=val ..." form.
    void print(std::ostream& os = std::clog) const;

    std::vector<std::string> names;         ///< Names/aliases of the action, e.g. {"read", "import", "load", "i"}.
    std::string              matchedName;   ///< The literal alias typed on the command line for this invocation (set during parsing).
    std::string              documentation; ///< One-line description shown in usage output.
    size_t                   anonymous;     ///< Index of the option receiving un-named values, or -1 if disallowed.
    bool                     greedy;        ///< If true, tokens with an unrecognized "name=" prefix also go to the anonymous option (see constructor docstring).
    std::vector<Option>      options;       ///< Options registered for this action.
    std::function<void()>    init;          ///< Callback invoked during parsing.
    std::function<void()>    run;           ///< Callback invoked during execution.
};// Action struct

// ==============================================================================================================

using ActListT = std::list<Action>;                  ///< Doubly-linked list of Actions (stable iterators).
using ActIterT = typename ActListT::iterator;        ///< Iterator into an ActListT.
using VecF     = std::vector<float>;                 ///< Convenience alias for a vector of floats.
using VecI     = std::vector<int>;                   ///< Convenience alias for a vector of ints.
using VecS     = std::vector<std::string>;           ///< Convenience alias for a vector of strings.

// ==============================================================================================================

/// @brief Key-value store mapping variable names to string values.
/// @details Used by the Processor (and by loops) to persist named variables
///          across expression evaluations. Provides built-in read-only constants
///          for "pi" and "e" when no user-defined value of that name exists.
class Memory
{
public:

    /// @brief Default constructor: produces an empty memory.
    Memory() = default;

    /// @brief Look up the value of a named variable.
    /// @param name Variable name (without the leading "$").
    /// @return The stored string value, or a string representation of pi/e
    ///         if @a name equals "pi" or "e" and no user value is set.
    /// @throw std::invalid_argument if the variable is not defined.
    std::string get(const std::string &name) const {
        auto it = mData.find(name);
        if (it == mData.end()) {
            if (name=="pi") {
                return std::to_string(std::atan(1)*4);
            } else if (name=="e") {
                return std::to_string(2.718281828459);
            } else {
                throw std::invalid_argument("Storage::get: undefined variable \""+name+"\"");
            }
        }
        return it->second;
    }
    /// @brief Erase all stored variables.
    void clear() {mData.clear();}
    /// @brief Erase a single variable by name (no-op if not present).
    void clear(const std::string &name) {mData.erase(name);}
    /// @brief Assign a string value to a variable.
    void set(const std::string &name, const std::string &value) {mData[name]=value;}
    /// @brief Assign a C-string value to a variable.
    void set(const std::string &name, const char *value) {mData[name]=value;}
    /// @brief Assign a numeric value to a variable (converted via std::to_string).
    template <typename T>
    void set(const std::string &name, const T &value) {mData[name]=std::to_string(value);}
    /// @brief Print all variables in lexicographic order as "name=value" lines.
    void print(std::ostream& os = std::clog) const {
        std::map<std::string, std::string> tmp(mData.begin(),mData.end());// sort output
        for (auto &d : tmp) os << d.first <<"="<<d.second<<std::endl;
    }
    /// @brief Returns the number of variables currently stored.
    size_t size() const {return mData.size();}
    /// @brief Returns true if a variable with the given name is currently set.
    bool isSet(const std::string &name) const {return mData.find(name)!=mData.end();}

private:

    std::unordered_map<std::string, std::string> mData; ///< Backing hash-map from name to value.
};// Memory

// ==============================================================================================================

/// @brief String stack used by the Processor for Forth-like Reverse Polish Notation evaluation.
/// @details Stack effects in the inline comments use the convention "before -- after",
///          where the rightmost token is the top of the stack.
class Stack {
public:

    /// @brief Default constructor; reserves a small initial capacity.
    Stack(){mData.reserve(10);}
    /// @brief Construct a stack from an initializer list (first element is the bottom).
    Stack(std::initializer_list<std::string> d) : mData(d.begin(), d.end()) {}
    /// @brief Returns the current depth (number of elements) of the stack.
    size_t depth() const {return mData.size();}
    /// @brief Returns true if the stack contains no elements.
    bool empty() const {return mData.empty();}
    /// @brief Equality test: returns true iff both stacks hold the same elements in the same order.
    bool operator==(const Stack &other) const {return mData == other.mData;}
    /// @brief Push @a s onto the top of the stack.
    void push(const std::string &s) {mData.push_back(s);}
    /// @brief Remove and return the top element.   Stack effect: y x -- y
    /// @throw std::invalid_argument if the stack is empty.
    std::string pop() {
        if (mData.empty()) throw std::invalid_argument("Stack::pop: empty stack");
        const std::string str = mData.back();
        mData.pop_back();
        return str;
    }
    /// @brief Remove the top element without returning it.   Stack effect: y x -- y
    /// @throw std::invalid_argument if the stack is empty.
    void drop() {
        if (mData.empty()) throw std::invalid_argument("Stack::drop: empty stack");
        mData.pop_back();
    }
    /// @brief Returns a mutable reference to the top element.
    /// @throw std::invalid_argument if the stack is empty.
    std::string& top() {
        if (mData.empty()) throw std::invalid_argument("Stack::top: empty stack");
        return mData.back();
    }
    /// @brief Returns a const reference to the top element without modifying the stack.
    /// @throw std::invalid_argument if the stack is empty.
    const std::string& peek() const {
        if (mData.empty()) throw std::invalid_argument("Stack::peak: empty stack");
        return mData.back();
    }
    /// @brief Duplicate the top element.   Stack effect: x -- x x
    /// @throw std::invalid_argument if the stack is empty.
    void dup() {
        if (mData.empty()) throw std::invalid_argument("Stack::dup: empty stack");
        mData.push_back(mData.back());
    }
    /// @brief Swap the two top elements.   Stack effect: y x -- x y
    /// @throw std::invalid_argument if the stack has fewer than 2 elements.
    void swap() {
        if (mData.size()<2) throw std::invalid_argument("Stack::swap: size<2");
        const size_t n = mData.size()-1;
        std::swap(mData[n], mData[n-1]);
    }
    /// @brief Remove the element just below the top.   Stack effect: y x -- x
    /// @throw std::invalid_argument if the stack has fewer than 2 elements.
    void nip() {
        if (mData.size()<2) throw std::invalid_argument("Stack::nip: size<2");
        mData.erase(mData.end()-2);
    }
    /// @brief Remove everything except the top element.   Stack effect: ... x -- x
    /// @throw std::invalid_argument if the stack is empty.
    void scrape() {
        if (mData.empty()) throw std::invalid_argument("Stack::scrape: empty stack");
        mData.erase(mData.begin(), mData.end()-1);
    }
    /// @brief Remove every element from the stack.
    void clear() {mData.clear();}
    /// @brief Copy the second element onto the top.   Stack effect: y x -- y x y
    /// @throw std::invalid_argument if the stack has fewer than 2 elements.
    void over() {
        if (mData.size()<2) throw std::invalid_argument("Stack::over: size<2");
        mData.push_back(mData[mData.size()-2]);
    }
    /// @brief Rotate the top three elements left.   Stack effect: z y x -- y x z
    /// @throw std::invalid_argument if the stack has fewer than 3 elements.
    void rot() {
        if (mData.size()<3) throw std::invalid_argument("Stack::rot: size<3");
        const size_t n = mData.size() - 1;
        std::swap(mData[n-2], mData[n  ]);
        std::swap(mData[n-2], mData[n-1]);
    }
    /// @brief Rotate the top three elements right.   Stack effect: z y x -- x z y
    /// @throw std::invalid_argument if the stack has fewer than 3 elements.
    void tuck() {
        if (mData.size()<3) throw std::invalid_argument("Stack::tuck: size<3");
        const size_t n = mData.size()-1;
        std::swap(mData[n-2], mData[n]);
        std::swap(mData[n-1], mData[n]);
    }
    /// @brief Print stack contents to @a os, comma-separated from bottom to top.
    void print(std::ostream& os = std::clog) const {
        if (mData.empty()) return;
        os << mData[0];
        for (size_t i=1; i<mData.size(); ++i) os << "," << mData[i];
    }

private:

    std::vector<std::string> mData; ///< Underlying contiguous storage; back() is the top of the stack.
};// Stack

// ==============================================================================================================

/// @brief   Implements a light-weight stack-oriented programming language (very loosely) inspired by Forth.
/// @details Specifically, it uses Reverse Polish Notation (RPN) to define operations that are evaluated
///          during parsing of the command-line arguments (options to be precise). Expressions are
///          delimited by "{" and "}" and tokens are separated by ":". Variables starting with "$" are
///          substituted from Memory; variables starting with "@" are stored to Memory.
///          See the README.md "Stack-based string expressions" section for the full language reference.
class Processor
{
public:

    /// @brief Push a numeric value onto the call stack (converted to string via std::to_string).
    template <typename T>
    void push(const T &t) {mCallStack.push(std::to_string(t));}
    /// @brief Push a string onto the call stack.
    void push(const std::string &s) {mCallStack.push(s);}
    /// @brief Replace the top stack entry with a numeric value (converted to string).
    template <typename T>
    void set(const T &t) {mCallStack.top() = std::to_string(t);}
    /// @brief Replace the top stack entry with "1" (true) or "0" (false).
    void set(bool t) {mCallStack.top() = t ? "1" : "0";}
    /// @brief Replace the top stack entry with a string.
    void set(const std::string &str) {mCallStack.top() = str;}
    /// @brief Replace the top stack entry with a C-string.
    void set(const char *str) {mCallStack.top() = str;}
    /// @brief Returns a mutable reference to the top stack entry.
    std::string& get() {return mCallStack.top();}
    /// @brief Read-only access to the memory used for "$name" and "@name" variables.
    const Memory& memory() const {return mMemory;}
    /// @brief Mutable access to the memory used for "$name" and "@name" variables.
    Memory& memory() {return mMemory;}
    /// @brief Register a new instruction with documentation and a callback function.
    /// @param name Instruction name (e.g. "+", "sqrt", "pad0").
    /// @param doc  Documentation string shown by the help() methods.
    /// @param func Callback executed when the instruction is invoked during evaluation.
    void add(const std::string &name, std::string &&doc, std::function<void()> &&func) {mInstructions[name]={std::move(doc),std::move(func)};}

    /// @brief Default constructor; registers all built-in instructions of the scripting language.
    Processor()
    {
        // file-name operations
        add("path","extract file path from string, e.g. {path/base0123.ext:path} -> {path}",
            [&](){mCallStack.top()=getPath(mCallStack.top());});
        add("file","extract file name from string, e.g. {path/base0123.ext:file} -> {base0123.ext}",
            [&](){mCallStack.top()=getFile(mCallStack.top());});
        add("name","extract file name without extension from string, e.g. {path/base0123.ext:name} -> {base0123}",
            [&](){mCallStack.top()=getName(mCallStack.top());});
        add("base","extract file base name from string, e.g. {path/base0123.ext:base -> {base}",
            [&](){mCallStack.top()=getBase(mCallStack.top());});
        add("number","extract file number from string, e.g. {path/base0123.ext:number} -> {0123}",
            [&](){mCallStack.top()=getNumber(mCallStack.top());});
        add("ext","extract file extension from string, e.g. {path/base0123.ext:ext} -> {ext}",
            [&](){mCallStack.top()=getExt(mCallStack.top());});
        add("replaceExt","replace file extension from string, e.g. {path/base0123.vdb:jpg:replaceExt} -> {path/base0123.jpg}",
            [&](){std::string ext = mCallStack.pop(), &file = mCallStack.top(); file = replaceExt(file, ext);});
        add("replacePath","replace file path from string, e.g. {tmp/base0123.vdb:path:replacePath} -> {path/base0123.vdb}",
            [&](){std::string path = mCallStack.pop(), &file = mCallStack.top(); file = replacePath(file, path);});

        // boolean operations
        add("==","returns true if the two top entries on the stack compare equal, e.g. {1:2:==} -> {0}",
            [&](){this->boolean(std::equal_to<>());});
        add("!=","returns true if the two top entries on the stack are not equal, e.g. {1:2:!=} -> {1}",
            [&](){this->boolean(std::not_equal_to<>());});
        add("<=","returns true if the two top entries on the stack are less than or equal, e.g. {1:2:<=} -> {1}",
            [&](){this->boolean(std::less_equal<>());});
        add(">=","returns true if the two top entries on the stack are greater than or equal, e.g. {1:2:>=} -> {0}",
            [&](){this->boolean(std::greater_equal<>());});
        add("<","returns true if the two top entries on the stack are less than, e.g. {1:2:<} -> {1}",
            [&](){this->boolean(std::less<>());});
        add(">","returns true if the two top entries on the stack are less than or equal, e.g. {1:2:>} -> {0}",
            [&](){this->boolean(std::greater<>());});
        add("!","logical negation, e.g. {1:!} -> {0}",
            [&](){this->set(!strToBool(mCallStack.top()));});
        add("|","logical or, e.g. {1:0:|} -> {1}",
            [&](){bool b=strToBool(mCallStack.pop());this->set(strToBool(mCallStack.top())||b);});
        add("&","logical and, e.g. {1:0:&} -> {0}",
            [&](){bool b=strToBool(mCallStack.pop());this->set(strToBool(mCallStack.top())&&b);});

        // math operations
        add("+","adds two top stack entries, e.g. {1:2:+} -> {3}",
            [&](){this->ab(std::plus<>());});
        add("-","subtracts two top stack entries, e.g. {1:2:-} -> {-1}",
            [&](){this->ab(std::minus<>());});
        add("*","multiplies two top stack entries, e.g. {1:2:*} -> {2}",
            [&](){this->ab(std::multiplies<>());});
        add("/","adds two top stack entries, e.g. {1.0:2.0:/} -> {0.5} and {1:2:/} -> {0}",
            [&](){this->ab(std::divides<>());});
        add("++","increment top stack entry, e.g. {1:++} -> {2}",
            [&](){this->a([](auto& x){return ++x;});});
        add("--","decrement top stack entry, e.g. {1:--} -> {0}",
            [&](){this->a([](auto& x){return --x;});});
        add("abs","absolute value, {-1:abs} -> {1}",
            [&](){this->a([](auto& x){return math::Abs(x);});});
        add("ceil","ceiling of floating point value, e.g. {0.5:ceil} -> {0.0}",
            [&](){this->a([](auto& x){return std::ceil(x);});});
        add("floor","floor of floating point value, e.g. {0.5:floor} -> {1.0}",
            [&](){this->a([](auto& x){return std::floor(x);});});
        add("pow2","square of value, e.g. {2:pow2} -> {4}",
            [&](){this->a([](auto& x){return math::Pow2(x);});});
        add("pow3","cube of value, e.g. {2:pow3} -> {8}",
            [&](){this->a([](auto& x){return math::Pow3(x);});});
        add("pow","power of vale, e.g. {2:3:pow} -> {8}",
            [&](){this->ab([](auto& a, auto& b){return math::Pow(a, b);});});
        add("min","minimum of two values, e.g. {1:2:min} -> {1}",
            [&](){this->ab([](auto& a, auto& b){return std::min(a, b);});});
        add("max","minimum of two values, e.g. {1:2:max} -> {2}",
            [&](){this->ab([](auto& a, auto& b){return std::max(a, b);});});
        add("neg","negative of value, e.g. {1:neg} -> {-1}",
            [&](){this->a([](auto& x){return -x;});});
        add("sign","sign of value, e.g. {-2:neg} -> {-1}",
            [&](){this->a([](auto& x){return (x > 0) - (x < 0);});});
        add("sin","sine of value, e.g. {$pi:sin} -> {0.0}",
            [&](){this->set(std::sin(strToFloat(mCallStack.top())));});
        add("cos","cosine of value, e.g. {$pi:cos} -> {-1.0}",
            [&](){this->set(std::cos(strToFloat(mCallStack.top())));});
        add("tan","tangent of value, e.g. {$pi:tan} -> {0.0}",
            [&](){this->set(std::tan(strToFloat(mCallStack.top())));});
        add("asin","inverse sine of value, e.g. {1:asin} -> {1.570796}",
            [&](){this->set(std::asin(strToFloat(mCallStack.top())));});
        add("acos","inverse cosine of value, e.g. {1:acos} -> {0.0}",
            [&](){this->set(std::acos(strToFloat(mCallStack.top())));});
        add("atan","inverse tangent of value, e.g. {1:atan} -> {0.785398}",
            [&](){this->set(std::atan(strToFloat(mCallStack.top())));});
        add("r2d","radian to degrees, e.g. {$pi:r2d} -> {180.0}",
            [&](){this->set(180.0f*strToFloat(mCallStack.top())/math::pi<float>());});
        add("d2r","degrees to radian, e.g. {180:d2r} -> {3.141593}",
            [&](){this->set(math::pi<float>()*strToFloat(mCallStack.top())/180.0f);});
        add("inv","inverse of value, e.g. {5:inv} -> {0.2}",
            [&](){this->set(1.0f/strToFloat(mCallStack.top()));});
        add("exp","exponential of value, e.g. {1:exp} -> {2.718282}",
            [&](){this->set(std::exp(strToFloat(mCallStack.top())));});
        add("ln","natural log of value, e.g. {1:ln} -> {0.0}",
            [&](){this->set(std::log(strToFloat(mCallStack.top())));});
        add("log","10 base log of value, e.g. {1:log} -> {0.0}",
            [&](){this->set(std::log10(strToFloat(mCallStack.top())));});
        add("sqrt","squareroot of value, e.g. {2:sqrt} -> {1.414214}",
            [&](){this->set(std::sqrt(strToFloat(mCallStack.top())));});
        add("to_int","convert value to integer, e.g. {1.2:to_int} -> {1}",
            [&](){this->set(int(strToFloat(mCallStack.top())));});
        add("to_float","convert value to floating point, e.g. {1:to_float} -> {1.0}",
            [&](){this->set(strToFloat(mCallStack.top()));});

        // stack operations
        add("dup","duplicates the top, i.e. pushes the top entry onto the stack, e.g. {x:dup} -> {x:x}",
            [&](){mCallStack.dup();});
        add("nip","remove the entry below the top, e.g. {x:y:nip} -> {y}",
            [&](){mCallStack.nip();});
        add("drop","remove/pop the top entry, e.g. {x:y:drop} -> {x}",
            [&](){mCallStack.drop();});
        add("swap","swap the two top entries, e.g. {x:y:swap} -> {y:x}",
            [&](){mCallStack.swap();});
        add("over","push second entry onto the top, e.g. {x:y:over} -> {x:y:x}",
            [&](){mCallStack.over();});
        add("rot","rotate three top entries left, e.g. {x:y:z:rot} -> {y:z:x}",
            [&](){mCallStack.rot();});
        add("tuck","rotate three top entries right, e.g. {x:y:z:tuck} -> {z:x:y}",
            [&](){mCallStack.tuck();});
        add("scrape","removed everything except for the top, e.g. {x:y:z:scrape} -> {z}",
            [&](){mCallStack.scrape();});
        add("clear","remove everything on the stack, e.g. {x:y:z:clear} -> {}",
            [&](){mCallStack.clear();});
        add("depth","push depth of stack onto the stack, e.g. {x:y:z:depth} -> {3}",
            [&](){this->push(mCallStack.depth());});
        add("squash","combines entire stack into the top, e.g. {x:y:z:squash} -> {x,y,z}",
            [&](){if (mCallStack.empty()) return;
                  std::stringstream ss;
                  mCallStack.print(ss);
                  mCallStack.scrape();
                  mCallStack.top()=ss.str();
        });

        // string operations
        add("lower","convert all characters in a string to lower case, e.g. {HeLlO:lower} -> {hello}",
            [&](){toLowerCase(mCallStack.top());});
        add("upper","convert all characters in a string to upper case, e.g. {HeLlO:upper} -> {HELLO}",
            [&](){toUpperCase(mCallStack.top());});
        add("length","push the number of characters in a string onto the stack, e.g. {foo bar:length} -> {7}",
            [&](){this->set(mCallStack.top().length());});
        add("replace","replace words in string, e.g. {for bar:a:b:replace} -> {foo bbr}",
            [&](){std::string b = mCallStack.pop(), a = mCallStack.pop(), &t = mCallStack.top();
                  for (size_t i=a.size(),j=b.size(),p=t.find(a); p!=std::string::npos; p=t.find(a,p+j)) t.replace(p,i,b);
        });
        add("erase","remove words in string, e.g. {foo bar:a:erase} -> {foo br}",
            [&](){std::string a = mCallStack.pop(), &t = mCallStack.top();
                  for (size_t p=t.find(a), n=a.size(); p!=std::string::npos; p=t.find(a,p)) t.erase(p,n);
        });
        add("append","append string to string, e.g. {foo:bar:append} -> {foobar}",
            [&](){const std::string str = mCallStack.pop();
                  mCallStack.top() += str;
        });
        add("tokenize","split a string according to a specific delimiter and push the tokens onto the stack e.g. foo,bar:,:tokenize -> foo:bar",
            [&](){const std::string delimiters = mCallStack.pop(), str = mCallStack.pop();
                  for (auto &s : tokenize(str, delimiters.c_str())) mCallStack.push(s);
        });
        add("match","test if a word matches a string, e.g. {sphere_01.vdb:sphere:match} -> {1}",
            [&](){std::string word = mCallStack.pop();
                  this->set(mCallStack.top().find(word) != std::string::npos);
        });

        add("is_set","returns true if a string has an associated value, e.g. {pi:is_set} ->{1}",
            [&](){this->set(mMemory.isSet(mCallStack.top()));});
        add("pad0","add zero-padding of a specified with to a string, e.g. {12:4:pad0} -> {0012}",
            [&](){const int w = strToInt(mCallStack.pop());
                  std::stringstream ss;
                  ss << std::setfill('0') << std::setw(w) << mCallStack.top();
                  mCallStack.top() = ss.str();
        });

        add("get","get the value of a variable from memory, e.g. {pi:get} -> {3.141593}, equal to {$pi}",
            [&](){mCallStack.top() = mMemory.get(mCallStack.top());});
        add("set","set a variable to a value and save it to memory, e.g. {1:G:set} -> {}, equal to {1:@G}",
            [&](){const std::string str = mCallStack.pop();
                  mMemory.set(str, mCallStack.pop());
        });
        add("date","date, e.g {date} -> {Sun Mar 27 19:31:16 2022} or {date: :_:replace} -> {Sun_Mar_27_19:31:55_2022}",
            [&](){std::time_t tmp = std::time(nullptr);
                  std::stringstream ss;
                  ss << std::asctime(std::localtime(&tmp));
                  this->push(ss.str());
        });
        add("uuid","random RFC 4122 v4 UUID hex string (thread-local PRNG), e.g. {uuid:.vdb:append} -> {821105a2-0e60-4a23-970d-0165e0ad4373.vdb}",
            [&](){this->push(uuid());}
        );

        // dummy entries for documentation
        add("$","get the value of a variable from memory, e.g. {$pi} -> {3.141593}", [](){});
        add("@","set a variable to a value and save it to memory, e.g. {1:@G} -> {}", [](){});
        add("if","if- and optional else-statement, e.g. {1:if(2)} -> {2} and {0:if(2?3)} -> {3}",[](){});
        add("switch","switch-statement, e.g. {2:switch(1:first?2:second?3:third)} -> {second}",[](){});
        add("quit","terminate evaluation, e.g. {1:2:+:quit:4:*} -> {3}",[](){});
    }

    /// @brief Performs syntax analysis on @a str and evaluates any embedded expressions.
    /// @details Each substring enclosed in "{}" is parsed as a sequence of colon-separated
    ///          tokens (values and instructions) and replaced in-place by the result.
    ///          Supports control-flow like if- and switch-statements. Modifies @a str directly.
    /// @param str Input/output string; rewritten with all expressions reduced to their values.
    /// @throw std::invalid_argument on any syntax error or unknown instruction.
    void operator()(std::string &str)
    {
        try {
        for (size_t pos = str.find_first_of("{}"); pos != std::string::npos; pos = str.find_first_of("{}", pos)) {
            if (str[pos]=='}') throw std::invalid_argument("Processor(): expected \"{\" before \"}\" in \""+str.substr(pos)+"\"");
            size_t end = str.find_first_of("{}", pos + 1);
            if (end == std::string::npos || str[end]=='{') throw std::invalid_argument("Processor(): nested \"{}\" is not allowed in \""+str.substr(pos)+"\"");
            for (size_t p=str.find_first_of(":}",pos+1), q=pos+1; p<=end; q=p+1, p=str.find_first_of(":}",q)) {
                if (p == q) {// ignores {:} and {::}
                    continue;
                } else if (str[q]=='$') {// get value
                    mCallStack.push(mMemory.get(str.substr(q + 1, p - q - 1)));
                } else if (str[q]=='@') {// set value
                    if (mCallStack.empty()) throw std::invalid_argument("Processor::(): cannot evaluate \""+str.substr(q,p-q)+"\" when the stack is empty");
                    mMemory.set(str.substr(q + 1, p - q - 1), mCallStack.pop());
                } else if (str.compare(q,3,"if(")==0) {// if-statement: 0|1:if(a) or 0|1:if(a?b)}
                    const size_t i = str.find_first_of("(){}", q+3);
                    if (str[i]!=')') throw std::invalid_argument("Processor():: missing \")\" in if-statement \""+str.substr(q)+"\"");
                    const auto v = tokenize(str.substr(q+3, i-q-3), "?");
                    if (v.size() == 1) {
                        if (strToBool(mCallStack.pop())) {
                            str.replace(q, i - q + 1, v[0]);
                        } else {
                            str.erase(q - 1, i - q + 2);// also erase the leading ':' character
                        }
                    } else if (v.size() == 2) {
                        str.replace(q, i - q + 1, v[strToBool(mCallStack.pop()) ? 0 : 1]);
                    } else {
                        throw std::invalid_argument("Processor():: invalid if-statement \""+str.substr(q)+"\"");
                    }
                    end = str.find('}', pos + 1);// needs to be recomputed since str was modified
                    p = q - 1;// rewind
                } else if (str.compare(q,4,"quit")==0) {// quit
                    break;
                } else if (str.compare(q,7,"switch(")==0) {//switch-statement: $1:switch(a:case_a?b:case_b?c:case_c)
                    const size_t i = str.find_first_of("(){}", q+7);
                    if (str[i]!=')') throw std::invalid_argument("Processor():: missing \")\" in switch-statement \""+str.substr(q)+"\"");
                    for (auto s : tokenize(str.substr(q+7, i-q-7), "?")) {
                        const size_t j = s.find(':');
                        if (j==std::string::npos) throw std::invalid_argument("Processor():: missing \":\" in switch-statement \""+str.substr(q)+"\"");
                        if (mCallStack.top() == s.substr(0,j)) {
                            str.replace(q, i - q + 1, s.substr(j + 1));
                            end = str.find('}', pos + 1);// needs to be recomputed since str was modified
                            p = q - 1;// rewind
                            mCallStack.drop();
                            break;
                        }
                    }
                    if (str.compare(q,7,"switch(")==0) throw std::invalid_argument("Processor():: no match in switch-statement \""+str.substr(q)+"\"");
                } else {// apply callback or push
                    const std::string s = str.substr(q, p - q);
                    auto it = mInstructions.find(s);
                    if (it != mInstructions.end()) {
                        it->second.callback();
                    } else {
                        mCallStack.push(s);
                    }
                }
            }// for-loop over ":" in string
            if (mCallStack.empty()) {// if call stack is empty clear inout string
                str.erase(pos, end-pos+1);
            } else if (mCallStack.depth()==1) {// if call stack has one entry replace it with the input string
                str.replace(pos, end-pos+1, mCallStack.pop());
            } else {// more than one entry in the call stack is considered an error
                std::stringstream ss;
                mCallStack.print(ss);
                throw std::invalid_argument("Processor::(): compute stack contains more than one entry: " + ss.str());
            }
        }// for-loop over "{}" in string
        } catch (const std::exception& e) {
            throw std::invalid_argument("Error evaluating \""+str+"\": "+e.what());
        }
    }
    /// @brief Non-mutating overload of operator(): evaluates @a str and returns the result.
    /// @return A new string with all "{}" expressions reduced.
    std::string operator()(const std::string &str)
    {
        std::string tmp = str;// copy
        (*this)(tmp);
        return tmp;
    }
    /// @brief Print documentation for every registered instruction in lexicographic order.
    void help(std::ostream& os = std::clog) const
    {
        std::set<std::string> vec;// print help in lexicographic order
        for (auto it=mInstructions.begin(); it!=mInstructions.end(); ++it) vec.insert(it->first);
        this->help(vec, os);
    }
    /// @brief Print documentation for a specific subset of instructions.
    /// @param vec Iterable of instruction names to look up.
    /// @throw std::invalid_argument if any name in @a vec is not a registered instruction.
    template <typename VecT>
    void help(const VecT &vec, std::ostream& os = std::clog) const
    {
        size_t w = 0;
        for (auto &s : vec) w = std::max(w, s.size());
        w += 2;
        for (auto &s : vec) {
            auto it = mInstructions.find(s);
            if (it != mInstructions.end()) {
                os << std::left << std::setw(static_cast<int>(w)) << it->first << it->second.doc << "\n\n";
            } else {
                throw std::invalid_argument("Processor::help:: unknown operation \"" + s + "\"");
            }
        }
    }

private:

    /// @brief Documentation string and callback function for a single registered instruction.
    struct Instruction {std::string doc; std::function<void()> callback;};
    /// @brief Hash-map type from instruction name to its Instruction record.
    using Instructions = std::unordered_map<std::string, Instruction>;

    Stack        mCallStack;    ///< Processor stack for data and intermediate results.
    Instructions mInstructions; ///< Map of all supported instructions, keyed by name.
    Memory       mMemory;       ///< Variable store used by "$name" / "@name" tokens.

    /// @brief Apply unary functor to the top element, parsed as either int or float.
    /// @throw std::invalid_argument if the top element parses as neither.
    template <typename OpT>
    void a(OpT op){
        union {std::int32_t i; float x;} A;
        if (isInt(mCallStack.top(), A.i)) {
            mCallStack.top() = std::to_string(op(A.i));
        } else if (isFloat(mCallStack.top(), A.x)) {
            mCallStack.top() = std::to_string(op(A.x));
        } else {
            throw std::invalid_argument("a: invalid argument \"" + mCallStack.top() + "\"");
        }
    }

    /// @brief Apply binary functor to the two top elements, parsed as either ints or floats.
    /// @throw std::invalid_argument if both elements do not share a numeric type.
    template <typename OpT>
    void ab(OpT op){
        union {std::int32_t i; float x;} A, B;
        const std::string str = mCallStack.pop();
        if (isInt(mCallStack.top(), A.i) && isInt(str, B.i)) {
            mCallStack.top() = std::to_string(op(A.i, B.i));
        } else if (isFloat(mCallStack.top(), A.x) && isFloat(str, B.x)) {
            mCallStack.top() = std::to_string(op(A.x, B.x));
        } else {
            throw std::invalid_argument("ab: invalid arguments \"" + mCallStack.top() + "\" and \"" + str + "\"");
        }
    }

    /// @brief Apply a boolean test (e.g. a == b) to the two top elements. If both parse as
    ///        the same numeric type the comparison is numeric, otherwise it is a string comparison.
    template <typename T>
    void boolean(T test){
        union {std::int32_t i; float x;} A, B;
        const std::string str = mCallStack.pop();
        if (isInt(mCallStack.top(), A.i) && isInt(str, B.i)) {
            mCallStack.top() = test(A.i, B.i) ? "1" : "0";
        } else if (isFloat(mCallStack.top(), A.x) && isFloat(str, B.x)) {
            mCallStack.top() = test(A.x, B.x) ? "1" : "0";
        } else {// string
            mCallStack.top() = test(mCallStack.top(), str) ? "1" : "0";
        }
    }
};// Processor class

// ==============================================================================================================

/// @brief Abstract base class for the iteration constructs (-for, -each, -files, -if).
/// @details A BaseLoop binds a loop variable name to a Memory store, exposes a position
///          counter accessible as "#name", and remembers the iterator pointing to the
///          first action inside the loop body. Concrete subclasses implement valid()
///          and next() to define the iteration policy.
struct BaseLoop
{
    /// @brief Constructor.
    /// @param s  Memory store used to publish the current loop value and counter.
    /// @param i  Iterator pointing at the action that opened the loop (e.g. "-for").
    /// @param n  Name of the loop variable (accessible as "$n", with counter as "$#n").
    BaseLoop(Memory &s, ActIterT i, const std::string &n) : memory(s), begin(i), name(n), pos(0) {}
    /// @brief Virtual destructor. Erases the loop variable and its counter from Memory.
    virtual ~BaseLoop() {
        memory.clear(name);
        memory.clear("#"+name);
    }
    /// @brief Returns true if the current iteration is valid (the body should execute).
    virtual bool valid() = 0;
    /// @brief Advance to the next iteration and return true if it is valid.
    virtual bool next() = 0;
    /// @brief Read the current loop value from Memory, converted to type @c T.
    template <typename T>
    T get() const { return strTo<T>(memory.get(name)); }
    /// @brief Publish a new loop value to Memory along with the updated position counter.
    template <typename T>
    void set(T v){
        memory.set(name, v);
        memory.set("#"+name, pos);
    }
    /// @brief Print "Processing: name = value, counter #name = pos" to @a os.
    void print(std::ostream& os = std::clog) const {
        os << "Processing: " << name << " = " << memory.get(name) << ", counter #" << name << " = " << pos <<std::endl;
    }

    Memory&     memory; ///< Reference to the Parser's Memory store.
    ActIterT    begin;  ///< Iterator marking the start of the loop body.
    std::string name;   ///< Loop-variable name.
    size_t      pos;    ///< Zero-based iteration counter.
    bool        quiet = false; ///< Suppress per-iteration "Processing: ..." print.
};// BaseLoop struct

// ==============================================================================================================

/// @brief Numeric for-loop driven by an inclusive lower bound, exclusive upper bound, and step.
/// @tparam T Numeric type of the loop variable (typically int or float).
template <typename T>
struct ForLoop : public BaseLoop
{
public:

    /// @brief Constructor.
    /// @param s Memory store used to publish the loop variable.
    /// @param i Iterator pointing at the -for action.
    /// @param n Name of the loop variable.
    /// @param v Vector containing {start, end} or {start, end, step}. Step defaults to 1.
    /// @throw std::invalid_argument if @a v has any size other than 2 or 3.
    ForLoop(Memory &s, ActIterT i, const std::string &n, const std::vector<T> &v) : BaseLoop(s, i, n), vec(1) {
        if (v.size()!=2 && v.size()!=3)  throw std::invalid_argument("ForLoop: expected two or three arguments, i=1,9 or i=1,9,2");
        for (size_t i=0; i<v.size(); ++i) vec[i] = v[i];
        if (this->valid()) this->set(vec[0]);
    }
    virtual ~ForLoop() {}
    /// @brief Returns true if the current value is still strictly less than the upper bound.
    bool valid() override {return vec[0] < vec[1];}
    /// @brief Advance the loop variable by the step and publish it to Memory.
    /// @return true if the new value is still in range.
    bool next() override {
        ++pos;
        vec[0] = this->template get<T>() + vec[2];// read from memory
        if (vec[0] < vec[1]) this->set(vec[0]);
        return vec[0] < vec[1];
    }

private:

    using BaseLoop::pos;
    math::Vec3<T> vec; ///< Triplet holding (current value, end value, step).
};// ForLoop struct

// ==============================================================================================================

/// @brief Loop that iterates over an explicit list of string values (the -each action).
class EachLoop : public BaseLoop
{
public:

    /// @brief Constructor.
    /// @param s Memory store used to publish the loop variable.
    /// @param i Iterator pointing at the -each action.
    /// @param n Name of the loop variable.
    /// @param v Ordered list of string values that the loop will iterate over.
    EachLoop(Memory &s, ActIterT i, const std::string &n, const VecS &v) : BaseLoop(s,i,n), vec(v.begin(), v.end()){
        if (this->valid()) this->set(vec[0]);
    }
    virtual ~EachLoop() {}
    /// @brief Returns true if the position counter is still within @c vec.
    bool valid() override {return pos < vec.size();}
    /// @brief Advance to the next entry in @c vec; returns true if such an entry exists.
    bool next() override {
        if (++pos < vec.size()) this->set(vec[pos]);
        return pos < vec.size();
    }

private:

    using BaseLoop::pos;
    const VecS vec; ///< Immutable list of values to iterate over.
};// EachLoop class

// ==============================================================================================================

/// @brief Loop that iterates over files matching extension, name-pattern, and size filters
///        within one or more directories (the -files action).
/// @tparam IterT Filesystem directory iterator type (e.g. std::filesystem::directory_iterator
///               or recursive_directory_iterator), selected to control recursion behavior.
template <typename IterT>
struct FilesLoop : public BaseLoop
{
public:

    /// @brief Constructor.
    /// @param s              Memory store used to publish the loop variable.
    /// @param i              Iterator pointing at the -files action.
    /// @param name           Name of the loop variable bound to each matching file path.
    /// @param pathString     One or more directories to traverse.
    /// @param fileExt        Acceptable file extensions (without leading dot); empty means accept all.
    /// @param includePattern Substring patterns; if non-empty, file name must contain at least one.
    /// @param excludePattern Substring patterns; if non-empty, file name must contain none of them.
    /// @param minFileSize    Minimum file size in bytes (inclusive).
    /// @param maxFileSize    Maximum file size in bytes (inclusive).
    FilesLoop(Memory &s, ActIterT i, const std::string &name,
              std::vector<std::string> &&pathString,
              std::vector<std::string> &&fileExt,
              std::vector<std::string> &&includePattern,
              std::vector<std::string> &&excludePattern,
              uint64_t minFileSize, uint64_t maxFileSize)
        : BaseLoop(s, i, name)
        , mPathNames(std::move(pathString))
        , mFileExt(std::move(fileExt))
        , mInclude(std::move(includePattern))
        , mExclude(std::move(excludePattern))
        , mMinFileSize(minFileSize)
        , mMaxFileSize(maxFileSize)
    {
        for (mPathIter = mPathNames.begin(); mPathIter != mPathNames.end(); ++mPathIter) {
            mFilePath = std::filesystem::path(*mPathIter);
            OPENVDB_ASSERT(std::filesystem::is_directory(mFilePath));
            mIter = IterT(mFilePath);
            mEnd  = std::filesystem::end(mIter);
            for(; !this->valid() && mIter != mEnd; ++mIter);
            if (mIter != mEnd) break;
        }
        if (mIter != mEnd) this->set(mIter->path().string());
    }
    virtual ~FilesLoop() {}
    /// @brief Returns true if the current file's extension matches any in @c mFileExt.
    bool matchExtensions() const {
        OPENVDB_ASSERT(mIter != mEnd && !mFileExt.empty());
        std::string ext = mIter->path().extension().string();// ".obj"
        if (ext.empty()) return false;// files has no extension
        ext = ext.substr(ext.find_first_not_of("."));// "obj"
        for (const auto &e : mFileExt) if (e == ext) return true;
        return false;
    }
    /// @brief Returns true if the current file's name contains at least one include pattern.
    bool includePatterns() const {
        OPENVDB_ASSERT(mIter != mEnd && !mInclude.empty());
        const std::string name = mIter->path().filename().string();// "file.obj"
        for (const auto &p : mInclude) if (name.find(p) != std::string::npos) return true;
        return false;
    }
    /// @brief Returns true if the current file's name contains none of the exclude patterns.
    bool excludePatterns() const {
        OPENVDB_ASSERT(mIter != mEnd && !mExclude.empty());
        const std::string name = mIter->path().filename().string();// "file.obj"
        for (const auto &p : mExclude) if (name.find(p) != std::string::npos) return false;
        return true;
    }
    /// @brief Returns true if the current entry is a regular file with size within bounds.
    bool fileSize() const {
        OPENVDB_ASSERT(mIter != mEnd);
        if (!std::filesystem::is_regular_file(mIter->path())) return false;// only loop over regular files
        const uint64_t size = file_size( mIter->path() );// fails if path is a directory
        return size >= mMinFileSize && size <= mMaxFileSize;
    }
    /// @brief Returns true if the current entry passes all configured filters.
    bool valid() override {
        if (mIter == mEnd) return false;
        if (!this->fileSize()) return false;
        if (!mFileExt.empty() && !this->matchExtensions()) return false;
        if (!mInclude.empty() && !this->includePatterns()) return false;
        if (!mExclude.empty() && !this->excludePatterns()) return false;
        return true;
    }
    /// @brief Advance to the next matching file, spanning directories if needed.
    /// @return true if another valid file was found.
    bool next() override {
        if (mPathIter == mPathNames.end()) return false;
        ++pos;
        ++mIter;
        while(mPathIter != mPathNames.end()) {
            for(; !this->valid() && mIter != mEnd; ++mIter);
            if (mIter != mEnd) break;// done
            ++mPathIter;
            if (mPathIter != mPathNames.end()) {
                mFilePath = std::filesystem::path(*mPathIter);
                OPENVDB_ASSERT(std::filesystem::is_directory(mFilePath));
                mIter = IterT(mFilePath);
                mEnd  = std::filesystem::end(mIter);
            }
        };
        if (mIter != mEnd) this->set(mIter->path().string());
        return mIter != mEnd;
    }

private:
 
    using BaseLoop::pos;
    const std::vector<std::string> mPathNames;            ///< Directories to traverse.
    std::filesystem::path mFilePath;                      ///< Path of the directory currently being scanned.
    const std::vector<std::string> mFileExt;              ///< Acceptable file extensions, or empty.
    const std::vector<std::string> mInclude;              ///< Required substring patterns, or empty.
    const std::vector<std::string> mExclude;              ///< Forbidden substring patterns, or empty.
    IterT mIter;                                          ///< Filesystem iterator over the current directory.
    IterT mEnd;                                           ///< End sentinel for @c mIter.
    std::vector<std::string>::const_iterator mPathIter;   ///< Iterator over @c mPathNames.
    const uint64_t mMinFileSize;                          ///< Minimum file size in bytes (inclusive).
    const uint64_t mMaxFileSize;                          ///< Maximum file size in bytes (inclusive).
};// FilesLoop struct

// ==============================================================================================================

/// @brief Degenerate "loop" that represents an if-block (the -if action).
/// @details Executes its body at most once: valid() always returns true on entry,
///          while next() always returns false to terminate after one pass.
class IfLoop : public BaseLoop
{
public:

    /// @brief Constructor; the if-block has no loop variable, hence the empty name.
    IfLoop(Memory &s, ActIterT i) : BaseLoop(s,i,"") {}
    virtual ~IfLoop() {}
    /// @brief Always returns true so the body executes once on entry.
    bool valid() override {return true;}
    /// @brief Always returns false; the if-block runs at most once.
    bool next() override {return false;}
};// IfLoop class

// ==============================================================================================================

/// @brief Control-flow scope for the -switch / -case construct.
/// @details A SwitchLoop is pushed by -switch and stays on the loop stack
///          until the matching outer -end. Each enclosed -case looks at the
///          stack top to find this SwitchLoop, compares its key against
///          @c selectedKey, and either opens a nested IfLoop (run the case
///          body) or skips ahead to the case's matching -end. The @c matched
///          flag is set the first time any case matches so that subsequent
///          cases are skipped and the catch-all "*"/"default" only fires
///          when nothing else did.
class SwitchLoop : public BaseLoop
{
public:
    std::string selectedKey; ///< Value of the -switch on= option for this scope.
    bool        matched;     ///< True once a case has matched in this switch.

    SwitchLoop(Memory &s, ActIterT i, std::string key)
        : BaseLoop(s, i, ""), selectedKey(std::move(key)), matched(false) {}
    virtual ~SwitchLoop() {}
    /// @brief Always returns true; the switch itself isn't a looping construct,
    ///        but the body between the matching -case and its -end runs once.
    bool valid() override { return true; }
    /// @brief Always returns false; the switch scope is single-pass.
    bool next() override  { return false; }
};// SwitchLoop class

// ==============================================================================================================

/// @brief Top-level command-line parser for vdb_tool.
/// @details Owns the registry of available actions, the ordered list of actions selected
///          on the command line, the active loop stack, and a Processor for evaluating
///          embedded "{}" expressions. Typical lifecycle:
///          @code
///              Parser p(defaults);
///              p.parse(argc, argv); // populates available 'actions'
///              p.finalize();        // validates loops, applies defaults
///              p.run();             // executes each action's 'run' callback in order
///          @endcode
struct Parser {
    /// @brief Constructor.
    /// @param def Global default options (applied to every matching action unless overridden).
    Parser(std::vector<Option> &&def);
    /// @brief Parse argv into the @c actions list, dispatching each token to an action's options.
    /// @throw std::invalid_argument if no arguments are supplied, an unknown action is encountered,
    ///        or "-for/-each/-files/-if" actions are not balanced by matching "-end" actions.
    void parse(int argc, char *argv[]);
    /// @brief Validate the parsed action list (loop matching, etc.) and prepare for run().
    void finalize();
    /// @brief Execute every action in @c actions, in order, honoring loops and if-blocks.
    void run();
    /// @brief Apply currently-stored defaults to the option list of the action at @c iter.
    void setDefaults();
    /// @brief Print every selected action in canonical form (useful for config-file output).
    void print(std::ostream& os = std::clog) const {for (auto &a : actions) a.print(os);}

    /// @brief Retrieve an option's value as a string, evaluating any "{}" expressions it contains.
    /// @throw std::invalid_argument if the current action has no option named @a name.
    std::string getStr(const std::string &name) const;
    /// @brief Retrieve an option's value converted to type @c T.
    template <typename T>
    T get(const std::string &name) const {return strTo<T>(this->getStr(name));}
    /// @brief Retrieve an option's value as a 3-component vector parsed with the given delimiters.
    /// @throw std::invalid_argument if the option does not parse into exactly 3 components.
    template <typename T>
    inline math::Vec3<T> getVec3(const std::string &name, const char* delimiters = "(),") const;
    /// @brief Retrieve an option's value as a variable-length vector parsed with the given delimiters.
    template <typename T>
    inline std::vector<T> getVec(const std::string &name, const char* delimiters = "(),") const;

    /// @brief Print usage information for a specific list of action names.
    void usage(const VecS &actions, bool brief) const;
    /// @brief Print usage for every action selected after the current one.
    void usage(bool brief) const {for (auto i = std::next(iter);i!=actions.end(); ++i) std::clog << this->usage(*i, brief);}
    /// @brief Print usage for every registered action.
    void usage_all(bool brief) const {for (const auto &a : available) std::clog << this->usage(a, brief);}
    /// @brief Format usage text for a single Action; brief omits per-option documentation.
    std::string usage(const Action &action, bool brief) const;
    /// @brief Register a new action with the parser.
    /// @param names     Names/aliases of the action (first entry is the canonical name).
    /// @param doc       Documentation string for the action.
    /// @param options   Options accepted by the action.
    /// @param parse     Callback invoked during parsing (e.g. for loop bookkeeping).
    /// @param run       Callback invoked during execution to perform the action.
    /// @param anonymous Index of the option receiving un-named values, or -1 to disallow them.
    /// @param greedy    If true, tokens whose "name=" prefix is not a recognized
    ///                  option are also appended (whole) to the anonymous
    ///                  option. Used when that option's value may contain '='
    ///                  (e.g. the kernel string in -calc / forValues).
    void addAction(std::vector<std::string> &&names,
                   std::string &&doc,
                   std::vector<Option>   &&options,
                   std::function<void()> &&parse,
                   std::function<void()> &&run,
                   size_t anonymous = -1,
                   bool   greedy    = false)
    {
      available.emplace_back(std::move(names), std::move(doc), std::move(options),
                             std::move(parse), std::move(run), anonymous, greedy);
    }

    /// @brief Returns a mutable reference to the action currently being processed.
    Action& getAction() {return *iter;}
    /// @brief Returns a const reference to the action currently being processed.
    const Action& getAction() const {return *iter;}
    /// @brief Print the current action to std::clog when verbose > 1.
    void printAction() const {if (verbose>1) iter->print();}
    /// @brief Search registered actions for names containing @a str (case-insensitive, leading '-' stripped).
    /// @return Multimap keyed by match position, used to suggest "did you mean" completions.
    std::multimap<size_t, std::string> closeMatches(const std::string &str) const;

    ActListT            available;                              ///< All registered (available) actions.
    ActListT            actions;                                ///< Actions selected by the user, in order.
    ActIterT            iter;                                   ///< Iterator pointing to the action being processed.
    std::unordered_map<std::string, ActIterT> hashMap;          ///< Name-to-iterator map for fast action lookup.
    std::list<std::shared_ptr<BaseLoop>> loops;                 ///< Active loop stack (innermost at the back).
    std::vector<Option> defaults;                               ///< Global default options applied to matching actions.
    int                 verbose;                                ///< Verbosity level (0=quiet, 1=info, 2=debug).
    mutable size_t      counter;                                ///< Counter validating balanced "-for/each/if" and "-end".
    mutable Processor   processor;                              ///< Processor evaluating "{}" expressions in option values.
    /// @brief Stack of currently-open loop/if scopes, in order of opening.
    ///        Each entry records the scope's keyword ("for", "each", "files",
    ///        "if") and the argv index where it was opened. Drained by `-end`.
    ///        Used to emit precise diagnostics for an unbalanced scope.
    mutable std::vector<std::pair<std::string, int>> mOpenLoops;
    /// @brief argv index of the action currently being initialized. Updated
    ///        by parse() before invoking each action's init() callback so the
    ///        loop-tracking lambdas know which command-line token they came
    ///        from. -1 outside parse().
    mutable int         mCurrentArgIdx = -1;
    /// @brief Optional callback to handle action errors. Called when an action throws.
    ///        If the callback returns true, the error is treated as fatal (re-thrown).
    ///        If it returns false, the error is logged as a skip and execution continues.
    ///        The callback receives (action_name, exception_message).
    std::function<bool(const std::string&, const std::string&)> onActionError = nullptr;
    /// @brief Optional callback applied to an option's value every time it is read,
    ///        after {} expression expansion but before the caller sees it. Receives the
    ///        option's name and its expanded value, which it may rewrite in place (e.g.
    ///        resolving a grid name to a stack age) using state the generic Parser has
    ///        no access to.
    /// @note  This deliberately post-processes a COPY on each read rather than rewriting
    ///        Option::value, so the stored source expression survives. Overwriting it
    ///        would break loops, where the same Action is revisited and must re-evaluate
    ///        its expressions each iteration (e.g. "-for v=0,3 -render vdb={$v} -end"),
    ///        and would also corrupt the source form written out by -write to a config file.
    std::function<void(const std::string&, std::string&)> onGetOption = nullptr;
};// Parser struct

// ==============================================================================================================

template <>
inline std::string Parser::get<std::string>(const std::string &name) const {return this->getStr(name);}

// ==============================================================================================================

template <>
inline std::vector<std::string> Parser::getVec<std::string>(const std::string &name, const char* delimiters) const
{
    return tokenize(this->getStr(name), delimiters);
}// Parser::getVec

// ==============================================================================================================

template <typename T>
std::vector<T> Parser::getVec(const std::string &name, const char* delimiters) const
{
    VecS v = this->getVec<std::string>(name, delimiters);
    std::vector<T> vec(v.size());
    for (size_t i=0; i<v.size(); ++i) vec[i] = strTo<T>(v[i]);
    return vec;
}// Parser::getVec

// ==============================================================================================================

template <typename T>
math::Vec3<T> Parser::getVec3(const std::string &name, const char* delimiters) const
{
    VecS v = this->getVec<std::string>(name, delimiters);
    if (v.size()!=3) throw std::invalid_argument(iter->names[0]+": Parser::getVec3: not a valid input "+name);
    return math::Vec3<T>(strTo<T>(v[0]), strTo<T>(v[1]), strTo<T>(v[2]));
}// Parser::getVec3

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif// VDB_TOOL_PARSER_HAS_BEEN_INCLUDED
