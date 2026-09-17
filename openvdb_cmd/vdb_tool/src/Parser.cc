// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file Parser.cc
/// @brief Out-of-line implementation of vdb_tool::Parser and vdb_tool::Action (see Parser.h).

#include "Parser.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

std::string Parser::getStr(const std::string &name) const
{
  OPENVDB_ASSERT(iter != actions.end());
  for (auto &opt : iter->options) {
      if (opt.name != name) continue;// linear search
      std::string str = opt.value;// deep copy since it might get modified by map
      processor(str);
      // Post-process the COPY, never opt.value itself -- see onGetOption's docstring.
      if (onGetOption) onGetOption(name, str);
      return str;
  }
  throw std::invalid_argument(iter->names[0]+": Parser::getStr: no option named \""+name+"\"");
}// Parser::getStr

// ==============================================================================================================

std::multimap<size_t, std::string> Parser::closeMatches(const std::string &str) const
{// Returns sorted map of available actions that look "close" to str. Leading
 // '-' is stripped before matching; the actual scoring (substring + Lev) lives
 // in Util.h's fuzzyMatch and is shared with Action::closeOptionMatches and
 // Calculator::suggestNames.
    const size_t pos = str.find_first_not_of("-");
    if (pos==std::string::npos) return {};// str is only '-' chars
    std::vector<std::string> candidates;
    for (const Action &a : available)
        for (const std::string &name : a.names) candidates.push_back(name);
    return fuzzyMatch(str.substr(pos), candidates);
}// Parser::closeMatches

// ==============================================================================================================

void Action::setOption(const std::string &str)
{
    // Greedy mode (set on actions whose anonymous option may take a value
    // containing '=', e.g. the kernel string in -calc / forValues): when no
    // explicit option matches, fall through to anonymous-append rather than
    // throwing "Invalid option".
    // Space-join when accumulating to a greedy anonymous option (preserves
    // a kernel like "a = 1+2" that the shell split into ["a", "=", "1+2"];
    // the default comma-join would corrupt it into "a,=,1+2"). Non-greedy
    // actions keep the comma separator since that's correct for file/grid
    // lists like "-read foo.vdb bar.vdb" → "files=foo.vdb,bar.vdb".
    auto appendValue = [&](Option &opt, const std::string &val) {
        if (greedy) {
            if (!opt.value.empty()) opt.value += ' ';
            opt.value += val;
        } else {
            opt.append(val);
        }
    };
    const size_t pos = str.find_first_of("={");// since expressions are only evaluated for values and not for names of values, we only search for '=' before expressions, which start with '{'
    if (pos == std::string::npos || str[pos]=='{') {// str has no "=" or it's an expression so append it to the value of the anonymous option
        if (anonymous>=options.size()) throw std::invalid_argument(names[0]+": does not support un-named option \""+str+"\"");
        appendValue(options[anonymous], str);
    } else if (anonymous<options.size() && str.compare(0, pos, options[anonymous].name) == 0) {
        appendValue(options[anonymous], str.substr(pos+1));
    } else {
        for (Option &opt : options) {
            // In greedy mode (where the anonymous option's value may legitimately
            // contain '=', e.g. a kernel assignment "v=v+1"), require an EXACT
            // option-name match. Otherwise short kernel identifiers that happen
            // to prefix a registered option name would silently land in that
            // option ("v=v+1" → "vdb=v+1"). Non-greedy actions keep the
            // convenient partial-match shortcut (e.g. "-erode r=2" → radius=2).
            if (greedy && opt.name.size() != pos) continue;
            if (opt.name.compare(0, pos, str, 0, pos) != 0) continue;// find first option with partial match
            opt.value = str.substr(pos+1);
            return;// done
        }
        for (Option &opt : options) {
            if (!opt.name.empty()) continue;// find first option with no name
            opt.name  = str.substr(0,pos);
            opt.value = str.substr(pos+1);
            return;// done
        }
        if (greedy && anonymous<options.size()) {// the anonymous option's value may contain '='; treat the whole token as anonymous.
            appendValue(options[anonymous], str);
            return;
        }
        std::stringstream ss;
        ss << names[0] << ": Invalid option: \"" << str << "\"\n";
        // Try a substring-based fuzzy match against the user-supplied name
        // prefix; if anything overlaps, lead with "Did you mean ..." so the
        // user's eye lands on the likely fix before the full option dump.
        const std::string badName = (pos == std::string::npos) ? str : str.substr(0, pos);
        const auto suggestions = this->closeOptionMatches(badName);
        if (!suggestions.empty()) {
            auto it = suggestions.begin();
            ss << "Did you mean: \"" << (it++)->second << "=...\"";
            while (it != suggestions.end()) ss << " or \"" << (it++)->second << "=...\"";
            ss << "?\n";
        }
        for (auto it = options.begin(); it != options.end();) {
            ss << "Valid options: \"" << it->name << "=" << (it++)->example << "\"";
            while (it != options.end()) ss << " or \"" << it->name << "=" << (it++)->example << "\"";
            ss << "\n";
        }
        throw std::invalid_argument(ss.str());
    }
}// Action::setOption

std::multimap<size_t, std::string> Action::closeOptionMatches(const std::string &name) const
{// Returns option names that look "close" to the user's input — handles both
 // typo (`radiuss=` → `radius`) and truncation (`rad=` → `radius`). Scoring
 // logic lives in Util.h's fuzzyMatch.
    std::vector<std::string> candidates;
    for (const Option &opt : options)
        if (!opt.name.empty()) candidates.push_back(opt.name);
    return fuzzyMatch(name, candidates);
}// Action::closeOptionMatches

// ==============================================================================================================

void Action::print(std::ostream& os) const
{
    os << "-" << names[0];
    for (auto &a : options) os << " " << a.name << "=" << a.value;
    os << std::endl;
}// Action::print

// ==============================================================================================================

Parser::Parser(std::vector<Option> &&def)
  : available()// vector of all available actions
  , actions()//   vector of all selected actions
  , iter()// iterator pointing to the current actions being processed
  , hashMap()
  , loops()// list of all for- and each-loops
  , defaults(def)// by default keep is set to false
  , verbose(1)// verbose level is set to 1 my default
  , counter(1)// 1-based global loop counter associated with 'G'
{
    this->addAction(
        {"eval"}, "evaluate string expression",
        {{"str", "", "{1:2:+}", "one or more strings to be processed by the stack-oriented programming language. Non-empty string outputs are printed to the terminal"},
         {"help", "", "*|+,-,...", "print a list of all or specified list operations each with brief documentation"}},
        [](){},
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "eval");
            if (!iter->options[1].value.empty()) {
                if (iter->options[1].value=="*") {
                    processor.help();
                } else {
                    processor.help(tokenize(iter->options[1].value, ","));
                }
            }
            std::string str = iter->options[0].value;// copy
            processor(str);// <- evaluate string
            if (!str.empty()) std::clog << str << std::endl;
            //for (auto s : tokenize(str, ",")) std::clog << s << std::endl;// split and print
        }, 0
    );

    this->addAction(
        {"calc", "math"}, "calculate string expression",
        {{"kernel", "", "3*sin(x)+y", "math expression. The \"kernel=\" prefix is OPTIONAL; the kernel may also be supplied as a bare positional argument, e.g. \"-calc '3*sin(x)+y'\" or \"-calc 'x=1+2'\". Input variables are read from the Processor's string memory (the same namespace used by -eval). Supports infix, RPN ($-prefixed), and infix multi-statement (;-separated) programs with assignment. Echoes the result only when the trailing statement is a plain expression; assignment-terminated kernels are silent since their outputs are already in memory."},
         {"file", "", "prog.txt", "read the expression from a file instead of the \"kernel\" option (useful for longer programs). If both are given, \"file\" takes precedence."}},
        [](){},// no pre-processing
        [&](){// post-process
            OPENVDB_ASSERT(iter->names[0] == "calc");
            std::string kernel = this->getStr("kernel");
            const std::string file = this->getStr("file");
            if (!file.empty()) kernel = readFileToString(file);
            if (kernel.empty()) return;
            Calculator cal;
            cal.compile(kernel);
            // Pull each input variable from the Processor's string memory
            // (the same namespace used by -eval). A name the kernel
            // references but the memory doesn't define is a user error —
            // fail loudly rather than silently substituting 0.
            auto &mem = processor.memory();
            std::vector<float> values(cal.variables().size());
            for (size_t i = 0; i < cal.variables().size(); ++i) {
                const std::string &name = cal.variables()[i];
                if (!mem.isSet(name)) {
                    throw std::invalid_argument(
                        "calc: kernel references undefined variable \"" + name +
                        "\" (set it first, e.g. -eval str='value:" + name + ":set')");
                }
                // Convert the stored string to a float. Memory holds arbitrary
                // strings (a {name:@name} typo, for instance, stores the
                // literal token rather than a number) so a clearer message
                // here helps users locate the offending variable and value.
                const std::string &raw = mem.get(name);
                try {
                    values[i] = strTo<float>(raw);
                } catch (const std::exception &) {
                    throw std::invalid_argument(
                        "calc: variable \"" + name + "\" in memory is not a "
                        "valid float (got \"" + raw + "\"). Did you mean to "
                        "store a number, e.g. -eval '{0:@" + name + "}'?");
                }
            }
            // evalAndRemember populates cal.memory() with every input,
            // every intermediate slot, and the trailing LHS name (if any).
            // We only mirror *outputs* (slots and the trailing-LHS result)
            // back into the Processor's memory; a pure read like kernel=n
            // must leave mem["n"] untouched, otherwise the float-formatted
            // "0.000000" rewrite breaks downstream int-typed consumers
            // such as -for loop variables and -if integer comparators.
            const float result = cal.evalAndRemember(values.data());
            auto isInput = [&](const std::string &name) {
                for (const auto &v : cal.variables()) if (v == name) return true;
                return false;
            };
            for (const auto &kv : cal.memory()) {
                const bool input  = isInput(kv.first);
                const bool result_assign = (kv.first == cal.resultName());
                // Skip entries that were read-only inputs and not also
                // reassigned via the trailing LHS.
                if (input && !result_assign) continue;
                mem.set(kv.first, std::to_string(kv.second));
            }
            // Only echo the result when the kernel ends in a plain
            // expression. If the trailing statement is an assignment
            // (`x = ...`), the value is already accessible via memory
            // and a separate -eval / -print would be redundant noise.
            if (cal.resultName().empty()) std::clog << result << std::endl;
        }, /*anonymous=*/0, /*greedy=*/true// kernel value may itself contain '=', so accept "-calc x=1+2" alongside "-calc kernel=x=1+2"
    );

    this->addAction(
        {"quiet"}, "disable printing to the terminal",{},
        [&](){verbose=0;},[&](){verbose=0;}
    );

    this->addAction(
        {"verbose"}, "print timing information to the terminal",{},
        [&](){verbose=1;},[&](){verbose=1;}
    );

    this->addAction(
        {"debug"}, "print debugging information to the terminal",{},
        [&](){verbose=2;},[&](){verbose=2;}
    );

    this->addAction(
        {"default"}, "define default values to be used by subsequent actions",
        std::vector<Option>(defaults), // using std::move produces error: moving a temporary object prevents copy elision
        [&](){OPENVDB_ASSERT(iter->names[0] == "default");
              std::vector<Option> &src = iter->options, &dst = defaults;
              OPENVDB_ASSERT(src.size() == dst.size());
              for (size_t i=0; i<src.size(); ++i) if (!src[i].value.empty()) dst[i].value = src[i].value;},
        [](){}
    );

    // Lambda function used to skip loops by forwarding iterator to matching -end.
    // Note, this function assumes that -for,-each,-if all have a matching -end, which
    // was enforced during parsing by increasing and decreasing "counter" and checking
    // that it never becomes negative and always ends up with a value of zero.
    auto skipToEnd = [](auto &it){
        for (int i = 1; i > 0;) {
            const std::string &name = (++it)->names[0];
            if (name == "end") {
                i -= 1;
            } else if (name == "for" || name == "each" || name == "if" || name == "files" ||
                       name == "switch" || name == "case") {
                i += 1;
            }
        }
        OPENVDB_ASSERT(it->names[0] == "end");
    };

    this->addAction(
        {"for"}, "start of for-loop over a user-defined loop variable and range.",
        {{"", "", "i=0,9|i=0,9,2", "define name of loop variable and its range."},
         {"quiet", "false", "0|1|false|true", "suppress per-iteration \"Processing: ...\" output for this loop"}},
        [&](){ ++counter; mOpenLoops.emplace_back("for", mCurrentArgIdx); },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "for");
            const std::string &name = iter->options[0].name;
            std::shared_ptr<BaseLoop> loop;
            try {
                loop=std::make_shared<ForLoop<int>>(processor.memory(), iter, name, this->getVec<int>(name,","));
            } catch (const std::invalid_argument &){
                loop=std::make_shared<ForLoop<float>>(processor.memory(), iter, name, this->getVec<float>(name,","));
            }
            if (loop->valid()) {
                loop->quiet = this->get<bool>("quiet");
                loops.push_back(loop);
                if (verbose && !loop->quiet) loop->print();
            } else {
                skipToEnd(iter);// skip to matching -end
            }
        }
    );

    // vdb_tool -quiet -files path="./dir" pattern="_1" ext="obj,ply,vdb" recur=1 -eval '{$file}'  -end
    // vdb_tool -files path=$HOME/dev/data recur=1 ext="obj,stl" -read '{$file}' -print -clear -end
    this->addAction(
        {"files"}, "start of files-loop in a directory.",
        {{"path", "", "/dir|path=/dir", "directory where file search is initiated (mandatory)"},
         {"extension", "", "\"obj,ply\"", "files must have one or more extensions"},
         {"include", "", "\"file1,file2\"", "include files that match one or more patterns"},
         {"exclude", "", "\"file1,file2\"", "exclude files that match one or more patterns"},
         {"min_size", "0", "1|1B|1KB|1MB|1GB|1TB", "minimum byte size, smaller files will be skipped"},
         {"max_size", "1TB", "1|1B|1KB|1MB|1GB|1TB", "maximum byte size, larger files will be skipped"},
         {"recursive", "0", "0|1|false|true", "recursive search of files into sub-directories."},
         {"quiet", "false", "0|1|false|true", "suppress per-iteration \"Processing: ...\" output for this loop"}},
        [&](){ ++counter; mOpenLoops.emplace_back("files", mCurrentArgIdx); },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "files");
            std::shared_ptr<BaseLoop> loop;
            if (this->get<bool>("recursive")) {
                using LoopT = FilesLoop<std::filesystem::recursive_directory_iterator>;
                loop = std::make_shared<LoopT>(processor.memory(), iter, "file", this->getVec<std::string>("path"),
                                               this->getVec<std::string>("extension",","),
                                               this->getVec<std::string>("include",","),
                                               this->getVec<std::string>("exclude",","),
                                               strSizeToByteSize(this->getStr("min_size")),
                                               strSizeToByteSize(this->getStr("max_size")));
            } else {
                using LoopT = FilesLoop<std::filesystem::directory_iterator>;
                loop = std::make_shared<LoopT>(processor.memory(), iter, "file", this->getVec<std::string>("path"),
                                               this->getVec<std::string>("extension",","),
                                               this->getVec<std::string>("include",","),
                                               this->getVec<std::string>("exclude",","),
                                               strSizeToByteSize(this->getStr("min_size")),
                                               strSizeToByteSize(this->getStr("max_size")));
            }
            if (loop->valid()) {
                loop->quiet = this->get<bool>("quiet");
                loops.push_back(loop);
                if (verbose && !loop->quiet) loop->print();
            } else {
                skipToEnd(iter);// skip to matching -end
            }
        }, 0// <-- "path=" is not required, ie both -files /path/dir and -files path=/path/dir are allowed
    );

    this->addAction(
        {"each"}, "start of each-loop over a user-defined loop variable and list of values.",
        {{"", "", "s=sphere,bunny,...", "defined name of loop variable and list of its values."},
         {"quiet", "false", "0|1|false|true", "suppress per-iteration \"Processing: ...\" output for this loop"}},
        [&](){ ++counter; mOpenLoops.emplace_back("each", mCurrentArgIdx); },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "each");
            const std::string &name = iter->options[0].name;
            auto loop = std::make_shared<EachLoop>(processor.memory(), iter, name, this->getVec<std::string>(name,","));
            if (loop->valid()) {
                loop->quiet = this->get<bool>("quiet");
                loops.push_back(loop);
                if (verbose && !loop->quiet) loop->print();
            } else {
                skipToEnd(iter);// skip to matching -end
            }
        }, 0
    );

    this->addAction(
        {"if"}, "start of if-scope. If the value of its option, named test, evaluates to false the entire scope is skipped",
        {{"test", "", "0|1|false|true | n>0 | a==b | ...", "boolean value used to test if-statement. Accepts 0/1/false/true (the existing literal form, including post-substitution {...:RPN} expressions), or any Calculator expression — infix or RPN — that evaluates to a numeric value (zero = false, non-zero = true). Variables are looked up in the Processor's string memory, the same namespace used by -eval / -calc, so e.g. -for n=0,10 -if 'n > 5' -... -end works."}},
        [&](){ ++counter; mOpenLoops.emplace_back("if", mCurrentArgIdx); },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "if");
            const std::string testStr = this->getStr("test");
            bool result;
            try {
                // Existing path: bare 0/1/false/true (and any RPN that already
                // reduced to such a literal via {...} substitution at parse time).
                result = strTo<bool>(testStr);
            } catch (const std::exception&) {
                // Fall back to the Calculator. Same compile path as -calc, same
                // variable-source convention (the Processor's memory), so an
                // -if inside a -for loop sees the loop variable on every pass.
                Calculator cal;
                cal.compile(testStr);
                auto& mem = processor.memory();
                std::vector<float> values(cal.variables().size());
                for (size_t i = 0; i < cal.variables().size(); ++i) {
                    const std::string& name = cal.variables()[i];
                    if (!mem.isSet(name)) {
                        throw std::invalid_argument(
                            "if: test expression \"" + testStr +
                            "\" references undefined variable \"" + name + "\"");
                    }
                    try {
                        values[i] = strTo<float>(mem.get(name));
                    } catch (const std::exception&) {
                        throw std::invalid_argument(
                            "if: variable \"" + name + "\" in memory is not a number (got \"" +
                            mem.get(name) + "\")");
                    }
                }
                result = cal.eval(values.data()) != 0.0f;
            }
            if (result) {
                loops.push_back(std::make_shared<IfLoop>(processor.memory(), iter));
            } else {
                skipToEnd(iter);// skip to matching -end
            }
        }, /*anonymous=*/0, /*greedy=*/true// test= may contain spaces (e.g. "n > 0"); collect trailing tokens with space-join so config-file expressions don't get comma-spliced
    );

    this->addAction(
        {"switch"}, "start of switch-scope. The selector value (on=) is compared against each enclosed -case's key; only the matching case body runs (or the '*'/'default' case if nothing else matched). Closed by -end.",
        {{"on", "", "1 | level_set | {$n}", "selector value compared against each enclosed -case key. {...} substitutions (e.g. {$n}) are evaluated before comparison. Numeric strings are compared by value so 1 matches 1.0."}},
        [&](){ ++counter; mOpenLoops.emplace_back("switch", mCurrentArgIdx); },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "switch");
            loops.push_back(std::make_shared<SwitchLoop>(processor.memory(), iter,
                                                          this->getStr("on")));
        }, /*anonymous=*/0, /*greedy=*/true// on= may contain spaces (e.g. "{$n} + 1")
    );

    this->addAction(
        {"case"}, "case branch inside a -switch scope. Body runs only if key matches the parent -switch's selector. Use key=* or key=default for a catch-all that fires when no earlier case matched. Closed by -end.",
        {{"key", "", "1 | foo | * | default", "case key compared against the parent -switch's selector. Numeric strings are compared by value (1 matches 1.0); otherwise strings are compared verbatim. The literals * and default mark the catch-all case."}},
        [&](){ ++counter; mOpenLoops.emplace_back("case", mCurrentArgIdx); },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "case");
            if (loops.empty()) {
                throw std::invalid_argument("-case must appear inside a -switch scope");
            }
            auto* sw = dynamic_cast<SwitchLoop*>(loops.back().get());
            if (sw == nullptr) {
                throw std::invalid_argument("-case must appear directly inside a -switch scope");
            }
            const std::string key = this->getStr("key");
            bool isMatch;
            if (key == "*" || key == "default") {
                isMatch = !sw->matched;// fires only if no earlier case in this switch hit
            } else {
                // Compare numerically when both sides parse as float, so 1 == 1.0,
                // 1.5 == 1.5e0, etc. Falls back to verbatim string comparison.
                try {
                    isMatch = (strTo<float>(key) == strTo<float>(sw->selectedKey));
                } catch (const std::exception&) {
                    isMatch = (key == sw->selectedKey);
                }
            }
            if (isMatch) {
                sw->matched = true;
                loops.push_back(std::make_shared<IfLoop>(processor.memory(), iter));
            } else {
                skipToEnd(iter);// skip this case body
            }
        }, /*anonymous=*/0, /*greedy=*/true
    );

    this->addAction(
        {"end"}, "marks the end scope of \"-for, -each, -files, -if, -switch and -case\" control actions", {},
        [&](){
            if (counter<=0) throw std::invalid_argument("Parser: -end must be preceded by -for, -each, -if, -switch or -case");
            --counter;
            if (!mOpenLoops.empty()) mOpenLoops.pop_back();
        },
        [&](){
            OPENVDB_ASSERT(iter->names[0] == "end");
            auto loop = loops.back();// current loop
            if (loop->next()) {// rewind loop
                iter = loop->begin;
                if (verbose && !loop->quiet) loop->print();
            } else {// exit loop
                loops.pop_back();
            }}
    );
}// Parser::Parser

// ==============================================================================================================

void Parser::run()
{
    for (iter=actions.begin(); iter!=actions.end(); ++iter) {
        if (onActionError) {
            // Centralized error handling: wrap the action callback
            try {
                iter->run();
            } catch (const std::exception& e) {
                const std::string &action_name = iter->names[0];
                const bool isFatal = onActionError(action_name, e.what());
                if (isFatal) {
                    throw std::invalid_argument(action_name + ": " + e.what());
                } else {
                    std::clog << action_name << ": skipping due to: " << e.what() << std::endl;
                }
            }
        } else {
            // Fallback if no error handler is registered (original behavior)
            iter->run();
        }
    }
}// Parser::run(

// ==============================================================================================================

void Parser::finalize()
{
    // sort available actions according to their primary name
    available.sort([](const Action &a, const Action &b){return a.names[0] < b.names[0];});

    // build hash table for accelerated random lookup
    for (auto it = available.begin(); it != available.end(); ++it) {
        for (const auto &name : it->names) hashMap.insert({name, it});
    }
}// Parser::finalize

// ==============================================================================================================

void Parser::parse(int argc, char *argv[])
{
    OPENVDB_ASSERT(!hashMap.empty());
    if (argc <= 1) throw std::invalid_argument("Parser: No arguments provided, try \"" + getFile(argv[0]) + " -help\"");
    counter = 0;// reset to check for matching {for,each,if}/end loops
    mOpenLoops.clear();

    // Build a command-line excerpt showing argv[badIdx] with a `^` caret line
    // underneath. Used to decorate setOption/unknown-action errors so the
    // user can see exactly which token on the command line was rejected.
    auto cmdLineCaret = [&](int badIdx) -> std::string {
        // Show a window of args around the bad one, at most ~80 chars total.
        // Prefer to keep argv[0] visible so the user sees their command name.
        constexpr size_t kMaxLen = 80;
        std::string line;
        line += "  in: ";
        const size_t caretPrefix = line.size();
        size_t caretCol = 0;
        for (int k = 0; k < argc; ++k) {
            if (k > 0) line += ' ';
            if (k == badIdx) caretCol = line.size();
            line += argv[k];
            if (line.size() > kMaxLen && k > badIdx) {// truncate after bad token
                line += " ...";
                break;
            }
        }
        if (line.size() > kMaxLen + 20) {// final truncation if window still huge
            // Trim near the head, keeping caret visible.
            const size_t keepFrom = (caretCol > 30) ? caretCol - 20 : caretPrefix;
            if (keepFrom > caretPrefix) {
                line = std::string("  in: ...") + line.substr(keepFrom);
                caretCol = caretCol - keepFrom + std::string("  in: ...").size();
            }
        }
        std::string caretLine(caretCol, ' ');
        caretLine += '^';
        return "\n" + line + "\n" + caretLine;
    };

    for (int i=1; i<argc; ++i) {
        const std::string str = argv[i];
        size_t pos = str.find_first_not_of("-");
        if (pos==std::string::npos) {
            throw std::invalid_argument(
                "Parser: expected an action but got \"" + str + "\"" + cmdLineCaret(i));
        }
        auto search = hashMap.find(str.substr(pos));//first remove all leading "-"
        if (search != hashMap.end()) {
            const int actionIdx = i;// remember the action keyword's position for diagnostics
            actions.push_back(*search->second);// copy construction of Action
            iter = std::prev(actions.end());// important
            iter->matchedName = str.substr(pos);// literal alias typed by the user
            while(i+1<argc && argv[i+1][0] != '-') {
                const int badIdx = i + 1;
                try {
                    iter->setOption(argv[++i]);
                } catch (const std::exception &e) {
                    throw std::invalid_argument(std::string(e.what()) + cmdLineCaret(badIdx));
                }
            }
            mCurrentArgIdx = actionIdx;// argv position of the action keyword (so -for points at "-for", not its option value)
            iter->init();// optional callback function unique to action
        } else {
            std::stringstream ss;
            ss << "Parser: Unsupported action: \"" << str << "\"\n";
            auto matches = this->closeMatches(str);
            for (auto it = matches.begin(); it != matches.end();) {
                ss << "Did you mean: \"-" << (it++)->second << "\"";
                while (it != matches.end()) ss << " or \"-" << (it++)->second << "\"";
                ss << "?";
            }
            ss << cmdLineCaret(i);
            throw std::invalid_argument(ss.str());
        }
    }// loop over all input arguments
    if (counter!=0) {
        // Build a message that names each unclosed scope and shows where it
        // was opened — far more actionable than the generic
        // "Unmatched pairing of {-for,-files,-each,-if} and -end".
        std::stringstream ss;
        ss << "Parser: " << mOpenLoops.size()
           << " unclosed scope" << (mOpenLoops.size() == 1 ? "" : "s")
           << " (missing -end)";
        for (const auto &kv : mOpenLoops) {
            ss << "\n  -" << kv.first;
            if (kv.second >= 0 && kv.second < argc) {
                ss << " at argv[" << kv.second << "] (\"" << argv[kv.second] << "\")";
            }
        }
        if (!mOpenLoops.empty() && mOpenLoops.back().second >= 0) {
            ss << cmdLineCaret(mOpenLoops.back().second);
        }
        throw std::invalid_argument(ss.str());
    }
    mCurrentArgIdx = -1;
}// Parser::parse

// ==============================================================================================================

void Parser::usage(const VecS &actions, bool brief) const
{
    for (const std::string &str : actions) {
        auto search = hashMap.find(str);
        if (search == hashMap.end()) {
            // Mirror the "Did you mean ...?" suggestion the parser gives for a
            // mistyped action on the command line, so "-help sphre" is as
            // helpful as "-sphre".
            // No action prefix here: the only caller (Tool::help) already
            // prepends "help: " in its catch handler.
            std::stringstream ss;
            ss << "unsupported action \"" << str << "\"";
            auto matches = this->closeMatches(str);
            for (auto it = matches.begin(); it != matches.end();) {
                ss << "\nDid you mean: \"-" << (it++)->second << "\"";
                while (it != matches.end()) ss << " or \"-" << (it++)->second << "\"";
                ss << "?";
            }
            throw std::invalid_argument(ss.str());
        }
        std::clog << this->usage(*search->second, brief);
    }
}// Parser::usage

// ==============================================================================================================

std::string Parser::usage(const Action &action, bool brief) const
{
    std::stringstream ss;
    const static int w = 17;
    auto op = [&](std::string line, size_t width, bool isSentence) {
        if (isSentence) {
            line[0] = static_cast<char>(std::toupper(line[0]));// capitalize std::string name, value, example, documentation;');// punctuate
        }
        width += w;
        const VecS words = tokenize(line, " ");
        for (size_t i=0, n=width; i<words.size(); ++i) {
            ss << words[i] << " ";
            n += words[i].size() + 1;
            if (i<words.size()-1 && n > 80) {// exclude last word
                ss << std::endl << std::left << std::setw(static_cast<int>(width)) << "";
                n = width;
            }
        }
        ss << std::endl;
    };
    for (const auto &name : action.names) ss << std::endl << std::left << std::setw(w) << "-" + name;
    std::string line;
    if (brief) {
        for (auto &opt : action.options) line+=opt.name+(opt.name!=""?"=":"")+opt.example+" ";
        if (line.empty()) line = "This action takes no options.";
        op(line, 0, false);
    } else {
        op(action.documentation, 0, true);
        size_t width = 0;
        for (const auto &opt : action.options) width = std::max(width, opt.name.size());
        width += 4;
        for (const auto &opt : action.options) {
            ss << std::endl << std::setw(w) << "" << std::setw(static_cast<int>(width));
            if (opt.name.empty()) {
                size_t p = opt.example.find('=');
                ss << opt.example.substr(0, p) << opt.example.substr(p+1);
            } else {
                ss << opt.name << opt.example;
            }
            ss << std::endl << std::left << std::setw(w+static_cast<int>(width)) << "";
            op(opt.documentation, width, true);
        }
    }
    return ss.str();
}// Parser::usage

// ==============================================================================================================

void Parser::setDefaults()
{
    for (auto &dst : iter->options) {
        if (dst.value.empty()) {// only set default value if the existing value is un-defined
            for (auto &src : defaults) {
                if (dst.name == src.name) {
                    dst.value = src.value;
                    break;//only breaks the innermost for-loop
                }
            }
        }
    }
}// Parser::setDefaults

// ==============================================================================================================

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
