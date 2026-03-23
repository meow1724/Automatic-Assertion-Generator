#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <regex>
#include <map>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <array>
#include <unistd.h>
#include "specification_llm.hpp"

using namespace std;

std::string config;

struct VoteResult{
    int vote;
    double confidence;
};

void open_config_file(){
    std::ifstream file("config.json");
    std::stringstream buffer;
    buffer << file.rdbuf();
    config = buffer.str();
}

string take_question_input(){
    ifstream file("input.txt");
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

string extractTextGemini(const string& json){
    size_t textPos = json.find("\"text\":");
    if(textPos == std::string::npos){
        std::cerr << "Gemini API error:\n" << json << std::endl;
        return "";
    }
    size_t start = json.find("\"", textPos + 7) + 1;
    size_t end = json.find("\"", start);
    return json.substr(start, end - start);
}

string extractTextGroq(const string& json){
    size_t contentPos = json.find("\"content\":\"");
    if(contentPos == string::npos){
        std::cerr << "Groq API error:\n" << json << std::endl;
        return "";
    }
    // move to start of actual text
    size_t start = contentPos + 11;
    // find end quote
    size_t end = start;
    while(end < json.size()){
        // stop at unescaped quote
        if(json[end] == '"' && json[end-1] != '\\') break;
        end++;
    }
    return json.substr(start, end - start);
}

string decodeJsonEscapes(const string& s) {
    string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'n') { out += '\n'; i++; }
            else if (c == 't') { out += '\t'; i++; }
            else if (c == 'r') { out += '\r'; i++; }
            else if (c == '\\') { out += '\\'; i++; }
            else if (c == '"') { out += '"'; i++; }
            else if (c == 'u' && i + 5 < s.size()) {
                string hex = s.substr(i + 2, 4);
                if (hex == "003e") out += '>';
                else if (hex == "003c") out += '<';
                else if (hex == "0026") out += '&';
                i += 5;
            }
            else out += s[i];
        }
        else out += s[i];
    }
    return out;
}

string cleanMarkdown(string text) {
    text = regex_replace(text, regex("#### "), "");
    text = regex_replace(text, regex("### "), "");
    text = regex_replace(text, regex("\\* "), "");
    text = regex_replace(text, regex("\\*\\*"),"");
    text = regex_replace(text, regex("---"), "");
    text = regex_replace(text, regex("```"), "");
    text = regex_replace(text, regex("```systemverilog"), "");
    text = regex_replace(text, regex("```smv"), "");
    text = regex_replace(text, regex("```nuxmv"), "");
    text = regex_replace(text, regex("\\|=>\\s*##1"), "|=>");
    text = regex_replace(text, regex("##\\[0:\\$\\]"), "");
    text = regex_replace(text, regex("\\|->\\s*\\((.*?)\\)\\s*\\|=>"), "|=> ($1 &&");
    text = regex_replace(text, regex("property[\\s\\S]*?endproperty;"), "");
    return text;
}

string fix_nuxmv_reserved_words(const string& text){
    string result = text;
    vector<pair<string,string>> replacements = {
        {"count",     "cnt"},
        {"abs",       "absv"},
        {"max",       "maxv"},
        {"min",       "minv"},
        {"sizeof",    "szof"},
        {"floor",     "flr"},
        {"extend",    "ext"},
        {"resize",    "rsz"},
        {"unsigned",  "usgn"},
        {"signed",    "sgnd"},
        {"integer",   "intgr"},
        {"word",      "wrd"},
        {"array",     "arr"},
        {"real",      "rl"},
        {"clock",     "clk"},
        {"process",   "proc"},
        {"self",      "slf"},
        {"of",        "ofv"},
        {"in",        "inp"},
        {"out",       "outp"},
        {"bool",      "boolv"},
        {"toint",     "tointv"},
        {"typedef",   "tdef"},
        {"PRED",      "predv"},
        {"MIRROR",    "mirv"},
    };

    for(auto& [reserved, safe] : replacements){
        regex word_re("\\b" + reserved + "\\b");
        result = regex_replace(result, word_re, safe);
    }

    return result;
}

string fix_nuxmv_syntax(const string& text){
    string result = text;
    result = regex_replace(result, regex("==="), "=");
    result = regex_replace(result, regex("!=="), "!=");
    result = regex_replace(result, regex("=="), "=");
    result = regex_replace(result, regex("<>"), "!=");
    {
        string tmp;
        for(size_t i = 0; i < result.size(); i++){
            if(i + 1 < result.size() && result[i] == '&' && result[i+1] == '&'){
                tmp += '&';
                i++; // skip second &
            } else { tmp += result[i]; }
        }
        result = tmp;
    }

    {
        string tmp;
        for(size_t i = 0; i < result.size(); i++){
            if(i + 1 < result.size() && result[i] == '|' && result[i+1] == '|'){
                tmp += '|';
                i++;
            } else { tmp += result[i]; }
        }
        result = tmp;
    }
    result = regex_replace(result, regex("~([a-zA-Z_(])"), "!$1");
    result = regex_replace(result, regex("\\+\\+"), "+ 1");
    result = regex_replace(result, regex("([a-zA-Z0-9_)])\\s*--\\s*"), "$1 - 1 ");
    regex ternary_paren("\\(([^?()]+)\\?\\s*([^:()]+):\\s*([^()]+)\\)");
    string prev;
    int safety = 0;
    do {
        prev = result;
        result = regex_replace(result, ternary_paren, "case $1 : $2; TRUE : $3; esac");
        safety++;
    } while(result != prev && safety < 20);

    return result;
}

string sanitize_nuxmv_model(const string& raw){
    stringstream ss(raw);
    string line;
    string result;
    bool in_model = false;
    vector<string> valid_tokens = {
        "MODULE", "VAR", "DEFINE", "ASSIGN",
        "init(", "next(", "case", "esac",
        "TRUE", "FALSE", ":=", "..",
        "boolean", ":", ";", "0", "1", "mod"
    };
    while(getline(ss, line)){
        size_t start = line.find_first_not_of(" \t\r\n");
        if(start == string::npos){
            if(in_model) result += "\n";
            continue;
        }
        string trimmed = line.substr(start);
        if(trimmed.find("```") == 0) continue;
        if(trimmed.find("--") == 0) continue;
        if(trimmed.find("//") == 0) continue;
        if(!in_model){
            if(trimmed.find("MODULE") == 0){
                in_model = true;
                result += trimmed + "\n";
            }
            continue;
        }
        if(trimmed.find("SPEC ") == 0 || trimmed.find("LTLSPEC ") == 0 || trimmed.find("FAIRNESS") == 0) continue;
        bool valid = false;
        if(trimmed == "VAR" || trimmed == "DEFINE" || trimmed == "ASSIGN"){
            valid = true;
        }
        else{
            for(auto& tok : valid_tokens){
                if(trimmed.find(tok) != string::npos){
                    valid = true;
                    break;
                }
            }
            if(!valid){
                for(char c : trimmed){
                    if(c == '&' || c == '|' || c == '=' || c == '(' ||
                       c == ')' || c == '+' || c == '-' || c == '!'){
                        valid = true;
                        break;
                    }
                }
            }
        }
        if(valid){
            result += "  " + trimmed + "\n";
        }
    }

    return result;
}

string sanitize_nuxmv_line(const string& raw){
    string s = raw;
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end2 = s.find_last_not_of(" \t\r\n");
    if(start == string::npos) return "";
    s = s.substr(start, end2 - start + 1);
    if(s.empty()) return "";
    if(s.find("SPEC") == string::npos && s.find("LTLSPEC") == string::npos && s.find("CTLSPEC") == string::npos && s.find("INVARSPEC") == string::npos) return "";
    while(!s.empty() && (s.front() == '-' || s.front() == '*' || s.front() == '#')) s = s.substr(1);
    start = s.find_first_not_of(" \t");
    if(start == string::npos) return "";
    s = s.substr(start);
    size_t commentPos = s.find(" --");
    if(commentPos != string::npos) s = s.substr(0, commentPos);
    end2 = s.find_last_not_of(" \t\r\n");
    if(end2 != string::npos) s = s.substr(0, end2 + 1);
    if(!s.empty() && s.back() != ';') s += ";";

    s = fix_nuxmv_reserved_words(s);
    s = regex_replace(s, regex("<>"), "!=");
    s = regex_replace(s, regex("==="), "=");
    s = regex_replace(s, regex("!=="), "!=");
    s = regex_replace(s, regex("=="), "=");
    {
        string tmp;
        for(size_t i = 0; i < s.size(); i++){
            if(i + 1 < s.size() && s[i] == '&' && s[i+1] == '&'){
                tmp += '&'; i++;
            } else { tmp += s[i]; }
        }
        s = tmp;
    }

    {
        string tmp;
        for(size_t i = 0; i < s.size(); i++){
            if(i + 1 < s.size() && s[i] == '|' && s[i+1] == '|'){
                tmp += '|'; i++;
            } else { tmp += s[i]; }
        }
        s = tmp;
    }
    s = regex_replace(s, regex("~([a-zA-Z_(])"), "!$1");
    s = regex_replace(s, regex("X\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*=\\s*"), "X($1) = ");
    s = regex_replace(s, regex("X\\s+!([a-zA-Z_][a-zA-Z0-9_]*)"), "X(!$1)");
    s = regex_replace(s, regex("X\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\)"), "X($1))");
    s = regex_replace(s, regex("X\\s+(TRUE|FALSE)"), "X($1)");
    regex broken_trail( "G\\s*\\((.+?)\\s*->\\s*X\\(([^)]+)\\)\\s*=\\s*([^)]+)\\)\\s*&\\s*\\(([^)]+)\\)\\s*;");
    s = regex_replace(s, broken_trail, "G ($1 & $4 -> X($2) = $3);");
    if(s.find("LTLSPEC") == string::npos && s.find("SPEC") != string::npos){
        s = regex_replace(s, regex("->\\s*X\\("), "-> AX(");
        s = regex_replace(s, regex("->\\s*X\\s+!"), "-> AX(!");
    }
    {
        regex array_access("[a-zA-Z_][a-zA-Z0-9_]*\\([a-zA-Z_][a-zA-Z0-9_]*\\)");
        regex valid_func("^(X|AX|AG|AF|EX|EG|EF|init|next)$");
        auto begin = sregex_iterator(s.begin(), s.end(), array_access);
        auto end = sregex_iterator();
        for(auto i = begin; i != end; ++i){
            string matched = (*i).str();
            size_t paren = matched.find('(');
            string func_name = matched.substr(0, paren);
            if(!regex_match(func_name, valid_func)){
                cerr << "[WARN] Dropping assertion with array-style access '" << matched << "': " << s << endl;
                return "";
            }
        }
    }

    // Drop assertions with nested temporal operators
    {
        regex nested_temporal("(AX|AG|AF|EX|EG|EF)\\s*\\([^)]*\\b(AX|AG|AF|EX|EG|EF|X|G|F|U)\\s*[\\(]");
        if(regex_search(s, nested_temporal)){
            cerr << "[WARN] Dropping assertion with nested temporal operators: " << s << endl;
            return "";
        }
    }
    if(s.find("?") != string::npos){
        regex ternary_check("\\?\\s*[^;]+\\s*:");
        if(regex_search(s, ternary_check)){
            cerr << "[WARN] Dropping assertion with ternary operator: " << s << endl;
            return "";
        }
    }

    return s;
}

string sanitize_assertions_text(const string& text){
    stringstream ss(text);
    string line;
    string result;
    while(getline(ss, line)){
        string sanitized = sanitize_nuxmv_line(line);
        if(!sanitized.empty()) result += sanitized + "\n";
    }
    return result;
}

string generate_specification(){
    string question = take_question_input();
    string specification_prompt = R"(You are a hardware formal modeling expert.
    Your task is to convert a hardware design description into a symbolic
    model compatible with the nuXmv model checker.

    The output must represent a deterministic finite-state transition system.

    STRICT OUTPUT RULES

    1. Output ONLY valid nuXmv code. No markdown, no comments, no explanations.
    2. Do NOT wrap the output in code fences like ``` or ```smv.
    3. The first line MUST be:

    MODULE main

    4. The model must contain these sections in order:

    MODULE main
    VAR
    DEFINE
    ASSIGN

    MODEL STRUCTURE RULES

    1. Every state variable must appear in VAR.
    2. Each variable must have a finite domain.
    3. Use boolean or bounded integer ranges only.
    4. Do not use arrays, reals, or infinite domains.
    5. Use push and pop as boolean input variables for queue operations.

    TRANSITION RULES

    1. All state updates must use next(variable).
    2. Each next(variable) must be deterministic.
    3. Use case statements for ALL conditional logic.
    4. Always include a TRUE branch in case statements.

    CONDITIONAL EXPRESSIONS

    nuXmv does NOT support the C-style ternary operator (? :).
    ALL conditional expressions must use case...esac blocks.

    CORRECT:
      count := case
        q0 & q1 : 2;
        q0 | q1 : 1;
        TRUE : 0;
      esac;

    WRONG (will cause syntax error):
      count := (q0 ? 1 : 0) + (q1 ? 1 : 0);

    DEFINE sections must also use case...esac for any conditionals.

    GUARD CONSISTENCY

    Every case branch that depends on a condition must include
    ALL necessary guards. For example if an operation is only
    valid when the queue is not empty, EVERY branch for that
    operation must include the not-empty guard, including
    wrap-around branches.

    Correct example for pop with wrap:
      pop & !empty & (head = 3) : 0;
      pop & !empty : head + 1;

    Wrong example (missing !empty on wrap branch):
      pop & (head = 3) : 0;
      pop & !empty : head + 1;

    INITIALIZATION

    Each state variable must have an init() value.

    SEMANTIC CONSISTENCY

    Ensure the following properties of the model:

    • State transitions preserve variable domains
    • Counters never overflow their bounds
    • Wrap-around logic must be explicit
    • Derived signals must be defined using DEFINE
    • Use explicit push/pop input signals to control enqueue/dequeue

    FORBIDDEN SYNTAX

    Do NOT use any of the following (they are NOT valid nuXmv):

    • Ternary operator: condition ? value1 : value2
    • C-style operators: ++, --, +=, &&, ||
    • Array indexing: arr[i]
    • SPEC, LTLSPEC, FAIRNESS
    • Comments, markdown, code fences, additional modules

    TASK

    Convert the following hardware description into a nuXmv symbolic model.)";
    string result = SpecificationLLM::call_specsllm(specification_prompt + "\n\n" + question);
    string text = extractTextGemini(result);
    string decoded = decodeJsonEscapes(text);
    string cleaned = cleanMarkdown(decoded);
    cleaned = fix_nuxmv_reserved_words(cleaned);
    cleaned = fix_nuxmv_syntax(cleaned);
    return sanitize_nuxmv_model(cleaned);
}

void write_specifications(string& specifications){
    ofstream file("specifications.txt");
    file << specifications;
    file.close();
}

string read_specifications(){
    ifstream file("specifications.txt");
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void write_assertions(string assertions){
    string sanitized = sanitize_assertions_text(assertions);
    ofstream file("assertions.txt");
    file << assertions;
    file.close();
}

void append_assertions(string assertions){
    string sanitized = sanitize_assertions_text(assertions);
    ofstream file("assertions.txt", ios::app);
    file << "\n\n" << assertions;
    file.close();
}

void generate_assertions_from_leader(){
    string spec = read_specifications();

    string leader_prompt = R"(You are a senior hardware formal verification engineer.
    You are given a hardware model written in nuXmv syntax.

    Your task is to generate formal properties that verify correctness
    of the design.

    The properties must reflect the actual transition logic of the model.

    STRICT OUTPUT FORMAT

    • Output ONLY nuXmv properties.
    • Each property must appear on its own line.
    • Each line must end with a semicolon.
    • Do NOT include comments, explanations, markdown, or code fences.

    ALLOWED PROPERTY TYPES

    SPEC <CTL formula>
    LTLSPEC <LTL formula>

    CRITICAL SYNTAX RULES

    In LTLSPEC (LTL), the next-state operator is X.
    X MUST ALWAYS be followed by parentheses around its argument:

    CORRECT:   X(count) = count + 1
    CORRECT:   X(tail) = 0
    CORRECT:   X(!empty)
    WRONG:     X count = count + 1
    WRONG:     X tail = 0
    WRONG:     X !empty

    In SPEC (CTL), the next-state operator is AX (not X):

    CORRECT:   SPEC AG (cond -> AX(var) = value);
    WRONG:     SPEC AG (cond -> X(var) = value);
    WRONG:     SPEC AG (cond -> X var = value);

    All conditions in an implication must be inside the
    temporal operator scope. Do NOT place conjuncts after
    the closing parenthesis of G(...) or AG(...).

    CORRECT:   LTLSPEC G (push & !full & tail = 0 -> X(mem0) = data_in);
    WRONG:     LTLSPEC G (push & !full -> X(mem0) = data_in) & (tail = 0);

    PROPERTY REQUIREMENTS

    Each property must:

    • reference variables defined in the model
    • correspond to actual design behavior
    • be meaningful for formal verification
    • not contradict the transition logic
    • not assume behavior of external inputs

    FORBIDDEN PROPERTIES

    Do NOT generate properties that:

    • restate variable bounds already enforced by types
    (example: SPEC AG count <= 4)

    • are tautologies
    (example: SPEC AG (x -> x))

    • assume environment behavior
    (example: LTLSPEC F push)

    • forbid simultaneous inputs unless the model forbids them

    • reference undefined signals

    PROPERTY CATEGORIES

    Generate properties covering:

    1. Safety invariants — illegal state combinations
    2. State transition correctness — verifying next-state updates
    3. Boundary conditions — overflow / underflow
    4. Control logic rules
    5. Wrap-around behavior
    6. Mutual exclusion conditions
    7. Progress properties when meaningful

    EXAMPLES

    SPEC AG !(full & empty);
    LTLSPEC G (push & !full -> X(!empty));
    LTLSPEC G (pop & !empty -> X(!full));
    LTLSPEC G (push & !full -> X(count) = count + 1);
    LTLSPEC G (push & !full & tail = 3 -> X(tail) = 0);

    LIMIT

    Generate between 10 and 20 properties.

    MODEL : 
    )" + spec;
    string leader_raw = SpecificationLLM::call_layer1_leader(leader_prompt);
    string text = extractTextGroq(leader_raw);
    string decoded_text = decodeJsonEscapes(text);
    string clean_leader_text = cleanMarkdown(decoded_text);
    clean_leader_text = fix_nuxmv_reserved_words(clean_leader_text);
    clean_leader_text = fix_nuxmv_syntax(clean_leader_text);

    write_assertions(clean_leader_text);
}

void generate_assertions_from_extenders(){
    string spec = read_specifications();
    ifstream file("assertions.txt");
    stringstream buffer;

    buffer << file.rdbuf();

    string existingAssertions = buffer.str();

    string extender_prompt = R"(You are a hardware formal verification expert.
    You are given:

    1. A nuXmv hardware model
    2. A set of existing formal properties

    Your task is to discover additional properties that verify behaviors
    not already covered.

    STRICT OUTPUT FORMAT

    • Output ONLY nuXmv properties
    • One property per line
    • Each line must end with a semicolon
    • Do NOT include comments, explanations, markdown, or code fences.

    CRITICAL SYNTAX RULES

    In LTLSPEC (LTL), the next-state operator is X.
    X MUST ALWAYS have parentheses around its argument:

    CORRECT:   X(count) = count + 1
    CORRECT:   X(tail) = 0
    CORRECT:   X(!empty)
    WRONG:     X count = count + 1
    WRONG:     X tail = 0

    In SPEC (CTL), the next-state operator is AX (not X):

    CORRECT:   SPEC AG (cond -> AX(var) = value);
    WRONG:     SPEC AG (cond -> X var = value);

    All conditions must be inside the temporal operator scope:

    CORRECT:   LTLSPEC G (a & b & c -> X(v) = expr);
    WRONG:     LTLSPEC G (a & b -> X(v) = expr) & (c);

    FORBIDDEN

    Do NOT generate:

    • duplicate properties
    • logically equivalent properties
    • reworded versions of existing properties
    • trivial invariants about variable bounds
    • tautologies
    • environment assumptions

    DISCOVERY STRATEGY

    Focus on behaviors not yet verified:

    1. corner cases
    2. simultaneous operations
    3. boundary transitions
    4. rare states
    5. wrap-around transitions
    6. data/control consistency

    TRANSITION ANALYSIS

    Inspect the ASSIGN section of the model.

    Derive properties based on next-state logic.

    Example reasoning:

    push & tail = 3 -> next(tail) = 0

    PROPERTY LIMIT

    Generate at most 10 new properties.)";

    extender_prompt += "\n\nMODEL:\n" + spec + "\n\nEXISTING PROPERTIES:\n" + existingAssertions;


    /* Example extender calls */
    /* We have assumed only 1 extender is there */
    string extender_raw = SpecificationLLM::call_layer1_extender(extender_prompt,1);
    string text = extractTextGroq(extender_raw);
    string decoded_text = decodeJsonEscapes(text);
    string clean_extender_text = cleanMarkdown(decoded_text);
    clean_extender_text = fix_nuxmv_reserved_words(clean_extender_text);
    clean_extender_text = fix_nuxmv_syntax(clean_extender_text);

    /* Append extender assertions */
    append_assertions(clean_extender_text);

    /* If you add more extenders in config,
    call them like this:

    for(int i=1;i<numLLMs;i++){
        call_layer1_extender(extender_prompt,i);
    }
    */
}

vector<string> read_assertions(){
    ifstream file("assertions.txt");
    vector<string> assertions;
    string line;
    while(getline(file, line)){
        if(line.empty()) continue;
        string sanitized = sanitize_nuxmv_line(line);
        if(!sanitized.empty()) assertions.push_back(sanitized);
    }
    return assertions;
}

string build_assertion_block(vector<string>& assertions){
    string block;
    for(int i=0;i<assertions.size();i++){
        block += "A" + to_string(i+1) + ": ";
        block += assertions[i];
        block += "\n";
    }
    return block;
}

string build_weighted_prompt(string block){
    string spec = read_specifications();
    string prompt = R"(You are a senior formal verification engineer.
    You will evaluate candidate nuXmv properties for a hardware model.

    You are given:

    1. A hardware symbolic model
    2. A list of candidate properties

    Each property must be evaluated for correctness.

    EVALUATION CRITERIA

    1. Syntax validity

    The property must be valid nuXmv syntax.

    Allowed forms:

    SPEC <CTL expression>
    LTLSPEC <LTL expression>

    Reject properties with malformed syntax.

    SPECIFIC SYNTAX CHECKS:

    • In LTLSPEC, the next-state operator X must have parentheses:
      CORRECT: X(count), X(!empty)
      WRONG: X count, X !empty
      If X lacks parentheses, vote = 0.

    • In SPEC (CTL), use AX not X for next state:
      CORRECT: SPEC AG (cond -> AX(var) = val)
      WRONG: SPEC AG (cond -> X(var) = val)

    • All conditions must be inside the temporal scope:
      CORRECT: G (a & b -> X(v) = e)
      WRONG: G (a -> X(v) = e) & (b)
      If a conjunct appears after the closing paren of G(), vote = 0.

    2. Variable validity

    All referenced variables must appear in the VAR or DEFINE sections.

    3. Transition consistency

    The property must not contradict the next-state logic
    defined in the ASSIGN section.

    Example rejection:

    If push & !full increases count,
    reject properties implying the opposite.

    4. Logical correctness

    Reject properties that:

    • encode impossible behaviors
    • misuse temporal operators
    • assume unspecified environment guarantees

    5. Tautologies

    Reject properties that are always true regardless of behavior.

    Examples:

    SPEC AG (x -> x)

    6. Redundancy

    If two properties express the same constraint,
    vote 0 for the redundant one.

    VOTING RULE

    vote = 1

    if the property is:

    • syntactically correct
    • logically meaningful
    • consistent with the model
    • useful for verification

    vote = 0

    if the property is:

    • incorrect
    • redundant
    • trivial
    • inconsistent with the model
    • has malformed X() syntax

    CONFIDENCE

    Return a number between 0 and 1.

    OUTPUT FORMAT

    Return STRICT JSON:

    {
    "A1":{"vote":1,"confidence":0.91},
    "A2":{"vote":0,"confidence":0.83}
    }

    Output JSON ONLY.
    )";

    prompt += "\n\nMODEL:\n" + spec + "\n\nASSERTIONS:\n" + block;
    return prompt;
}

map<int, VoteResult> parse_weighted_votes(string response){
    map<int, VoteResult> result;
    regex pattern("\"A([0-9]+)\"\\s*:\\s*\\{\\s*\"vote\"\\s*:\\s*([01])\\s*,\\s*\"confidence\"\\s*:\\s*([0-9\\.]+)\\s*\\}");
    auto begin = sregex_iterator(response.begin(), response.end(), pattern);
    auto end = sregex_iterator();
    for(auto i = begin; i != end; ++i){
        int index = stoi((*i)[1]);
        int vote = stoi((*i)[2]);
        double conf = stod((*i)[3]);
        result[index] = {vote, conf};
    }
    return result;
}

bool weighted_decision(vector<VoteResult> votes){
    if(votes.empty()) return false;
    double weighted_sum = 0;
    double total_conf = 0;
    for(auto &v : votes){
        weighted_sum += v.vote * v.confidence;
        total_conf += v.confidence;
    }
    if(total_conf == 0) return false;
    double score = weighted_sum / total_conf;
    return score >= 0.5;
}

void verify_assertions_weighted(){
    vector<string> assertions = read_assertions();
    string block = build_assertion_block(assertions);
    string prompt = build_weighted_prompt(block);
    int numLLMs = SpecificationLLM::count_models("layer2Llms");
    vector<vector<VoteResult>> votes(assertions.size());
    for(int i=0;i<numLLMs;i++){
        string raw = SpecificationLLM::call_layer2(prompt,i);
        string text = extractTextGroq(raw);
        string decoded = decodeJsonEscapes(text);
        auto parsed = parse_weighted_votes(decoded);
        for(auto &p : parsed){
            int index = p.first - 1;
            if(index >= 0 && index < (int)assertions.size()) votes[index].push_back(p.second);
        }
    }
    ofstream out("verified_assertions.txt");
    for(int i=0;i<assertions.size();i++){
        if(weighted_decision(votes[i])) out << assertions[i] << "\n";
    }
}

vector<string> read_verified_assertions(){
    ifstream file("verified_assertions.txt");
    vector<string> assertions;
    string line;
    while(getline(file,line)){
        if(line.empty()) continue;
        string sanitized = sanitize_nuxmv_line(line);
        if(!sanitized.empty()) assertions.push_back(sanitized);
    }
    return assertions;
}

string create_combined_smv_model(const string& model, const vector<string>& assertions){
    string filename = "combined_model.smv";
    ofstream out(filename);
    string clean_model = sanitize_nuxmv_model(model);
    out << clean_model << "\n";
    for(const auto& a : assertions){
        string sanitized = sanitize_nuxmv_line(a);
        if(!sanitized.empty()) out << sanitized << "\n";
    }
    out.close();
    return filename;
}

string run_nuXmv(const string& filename){
    string command = "nuXmv " + filename;
    array<char, 128> buffer;
    string result;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "ERROR";
    while(fgets(buffer.data(), buffer.size(), pipe) != nullptr){
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

map<int, pair<string,string>> parse_nuxmv_output(const string& output){
    map<int, pair<string,string>> results;
    regex spec_pattern("-- specification .*? is (true|false)");
    vector<string> lines;
    stringstream ss(output);
    string line;
    while(getline(ss,line)) lines.push_back(line);
    int spec_counter = 0;
    int current_index = -1;
    bool capturing_trace = false;
    string trace;
    for(const auto& l : lines){
        smatch match;
        if(regex_search(l, match, spec_pattern)){
            capturing_trace = false;
            spec_counter++;
            current_index = spec_counter;
            string result = match[1];
            if(result == "true") results[current_index] = {"PASS",""};
            else{
                results[current_index] = {"FAIL",""};
                capturing_trace = true;
                trace.clear();
            }
            continue;
        }
        if(capturing_trace){
            if(l.find("-- specification") != string::npos){
                capturing_trace = false;
            }
            else{
                trace += l + "\n";
                results[current_index].second = trace;
            }
        }
    }

    return results;
}

void layer3_model_checking(){
    string model = read_specifications();
    vector<string> assertions = read_verified_assertions();
    if(assertions.empty()){
        cerr << "[ERROR] No assertions found in verified_assertions.txt" << endl;
        return;
    }
    cout << "Running Layer 3 Model Checking..." << endl;
    cout << "Found " << assertions.size() << " assertions to verify." << endl;
    string smv_file = create_combined_smv_model(model, assertions);
    string output = run_nuXmv(smv_file);
    auto results = parse_nuxmv_output(output);

    // File 1: Everything (PASS + FAIL + ERROR) — full report
    ofstream out("formal_verified_assertions.txt");

    // File 2: Only PASS — clean assertions for Layer 4 subsumption/clustering
    ofstream passed("passed_formal_verified_assertions.txt");

    // File 3: Only FAIL — with counterexamples for debugging
    ofstream failed("Failed_formal_verified_assertions.txt");

    int pass_count = 0;
    int fail_count = 0;

    for(int i=0;i<(int)assertions.size();i++){
        int idx = i+1;
        if(results.count(idx) && results[idx].first == "PASS"){
            // Full report
            out << "PASS: " << assertions[i] << "\n\n";

            // Passed file: clean assertion only (no "PASS:" prefix), ready for Layer 4
            passed << assertions[i] << "\n";

            cout << "PASS: " << assertions[i] << "\n";
            pass_count++;
        }
        else if(results.count(idx) && results[idx].first == "FAIL"){
            // Full report
            out << "FAIL: " << assertions[i] << "\n";
            out << "Counterexample:\n";
            out << results[idx].second << "\n\n";

            // Failed file: assertion + counterexample
            failed << "FAIL: " << assertions[i] << "\n";
            failed << "Counterexample:\n";
            failed << results[idx].second << "\n\n";

            cout << "FAIL: " << assertions[i] << "\n";
            fail_count++;
        }
        else{
            out << "ERROR: " << assertions[i] << " (nuXmv did not report result)\n\n";
            cout << "ERROR: " << assertions[i] << " (no result from nuXmv)\n";
        }
    }

    out.close();
    passed.close();
    failed.close();

    cout << "\nLayer 3 Summary: " << pass_count << " PASS, " << fail_count << " FAIL out of "
         << assertions.size() << " total assertions." << endl;
    cout << "Results written to:" << endl;
    cout << "  formal_verified_assertions.txt          (full report)" << endl;
    cout << "  passed_formal_verified_assertions.txt    (PASS only, for Layer 4)" << endl;
    cout << "  Failed_formal_verified_assertions.txt    (FAIL + counterexamples)" << endl;
}

// =====================================================================
// LAYER 4: Subsumption & Clustering
// =====================================================================

/*
    Read the passed assertions from Layer 3 output.
*/
vector<string> read_passed_assertions(){
    ifstream file("passed_formal_verified_assertions.txt");
    vector<string> assertions;
    string line;
    while(getline(file, line)){
        if(line.empty()) continue;
        size_t s = line.find_first_not_of(" \t\r\n");
        size_t e = line.find_last_not_of(" \t\r\n");
        if(s == string::npos) continue;
        string trimmed = line.substr(s, e - s + 1);
        if(!trimmed.empty()) assertions.push_back(trimmed);
    }
    return assertions;
}
 
/*
    Single LLM call: send ALL assertions + model to the LLM.
    Ask it to do BOTH clustering and subsumption in one shot.
    Returns JSON with cluster assignments and subsumption removals.
*/
struct Layer4Result {
    map<string, vector<int>> clusters;       // cluster_name -> list of assertion indices (1-based)
    vector<int> subsumed_indices;             // 1-based indices to remove
    map<int, string> subsumption_reasons;     // index -> reason
};
 
Layer4Result run_layer4_llm(const vector<string>& assertions){
    Layer4Result result;
 
    // Build numbered assertion block
    string block;
    for(int i = 0; i < (int)assertions.size(); i++){
        block += "P" + to_string(i+1) + ": " + assertions[i] + "\n";
    }
 
    string model_spec = read_specifications();
 
    string prompt = R"(You are a formal verification expert specializing in nuXmv temporal logic.
 
You are given a hardware model and a set of FORMALLY VERIFIED properties (all TRUE on the model, checked by nuXmv).
 
You must perform TWO tasks in a SINGLE response:
 
TASK 1: CLUSTERING
Group the properties by what aspect of the design they verify.
Use descriptive cluster names like:
- "safety_invariants" for mutual exclusion, impossibility constraints
- "count_transitions" for counter increment/decrement/stability
- "head_pointer" for head advancement and wrap-around
- "tail_pointer" for tail advancement and wrap-around
- "memory_write" for data slot write operations (q0, q1, q2, q3 being set)
- "memory_clear" for data slot clear operations (q0, q1, q2, q3 being cleared)
- "boundary" for bound checks and boundary conditions
Use your judgment. A property belongs to exactly ONE cluster.
 
TASK 2: SUBSUMPTION
Within and across clusters, identify if any property is logically subsumed (implied) by another.
Property A SUBSUMES property B if A being true guarantees B is true.
 
Rules:
- Stricter guard + same conclusion = the weaker-guard version is subsumed
  Example: G(a & b -> X(v)=e) makes G(a -> X(v)=e) redundant
- Exact value property subsumes a bound check on the same variable
- DO NOT remove a property if it covers a different variable or different edge case
- BE CONSERVATIVE: when in doubt, KEEP both
 
OUTPUT FORMAT — Return STRICT JSON only:
 
{
  "clusters": {
    "safety_invariants": [1, 12],
    "count_transitions": [4, 6, 7, 8],
    "head_pointer": [5, 9, 10, 11],
    "tail_pointer": [3, 13, 14],
    "memory_write": [15, 16, 17],
    "memory_clear": [18, 19, 20]
  },
  "subsumed": [
    {"remove": 7, "kept": 4, "reason": "P4 has stricter guard with same conclusion"},
    {"remove": 12, "kept": 1, "reason": "P1 safety invariant already covers this"}
  ]
}
 
Rules for JSON:
- "clusters": map of cluster_name -> list of property numbers (P1=1, P2=2, etc.)
- Every property must appear in exactly one cluster
- "subsumed": list of removals. "remove" is the redundant one, "kept" is the stronger one
- If nothing is subsumed, use "subsumed": []
- Output ONLY JSON. No text before or after.
 
MODEL:
)" + model_spec + "\n\nPROPERTIES (" + to_string(assertions.size()) + " total):\n" + block;
 
    // Single LLM call
    cout << "Sending " << assertions.size() << " assertions to Layer 4 LLM..." << endl;
    string raw = SpecificationLLM::call_layer4(prompt, 0);
    string text = extractTextGroq(raw);
    string decoded = decodeJsonEscapes(text);
 
    cerr << "[DEBUG] Layer 4 LLM response:\n" << decoded << endl;
 
    // Parse clusters: "cluster_name": [1, 2, 3]
    // Find each cluster and its indices
    regex cluster_re("\"([a-zA-Z_][a-zA-Z0-9_]*)\"\\s*:\\s*\\[([0-9,\\s]+)\\]");
    string clusters_section = decoded;
 
    // Find the "clusters" object
    size_t clusters_start = decoded.find("\"clusters\"");
    size_t subsumed_start = decoded.find("\"subsumed\"");
 
    if(clusters_start != string::npos){
        // Extract just the clusters section
        string csec;
        if(subsumed_start != string::npos && subsumed_start > clusters_start)
            csec = decoded.substr(clusters_start, subsumed_start - clusters_start);
        else
            csec = decoded.substr(clusters_start);
 
        auto cb = sregex_iterator(csec.begin(), csec.end(), cluster_re);
        auto ce = sregex_iterator();
        for(auto i = cb; i != ce; ++i){
            string cname = (*i)[1];
            string indices_str = (*i)[2];
 
            // Skip "clusters" itself as a key
            if(cname == "clusters") continue;
 
            // Parse the comma-separated indices
            regex num_re("([0-9]+)");
            auto nb = sregex_iterator(indices_str.begin(), indices_str.end(), num_re);
            auto ne = sregex_iterator();
            for(auto j = nb; j != ne; ++j){
                int idx = stoi((*j)[1]);
                if(idx >= 1 && idx <= (int)assertions.size()){
                    result.clusters[cname].push_back(idx);
                }
            }
        }
    }
 
    // Parse subsumed: {"remove": 7, "kept": 4, "reason": "..."}
    regex remove_re("\"remove\"\\s*:\\s*([0-9]+)");
    regex reason_re("\"reason\"\\s*:\\s*\"([^\"]+)\"");
 
    // Find all remove entries
    size_t pos = 0;
    if(subsumed_start != string::npos) pos = subsumed_start;
    string sub_section = decoded.substr(pos);
 
    auto rb = sregex_iterator(sub_section.begin(), sub_section.end(), remove_re);
    auto re2 = sregex_iterator();
    for(auto i = rb; i != re2; ++i){
        int idx = stoi((*i)[1]);
        if(idx >= 1 && idx <= (int)assertions.size()){
            result.subsumed_indices.push_back(idx);
        }
    }
 
    // Parse reasons
    auto rsb = sregex_iterator(sub_section.begin(), sub_section.end(), reason_re);
    auto rse = sregex_iterator();
    int reason_idx = 0;
    for(auto i = rsb; i != rse; ++i){
        if(reason_idx < (int)result.subsumed_indices.size()){
            result.subsumption_reasons[result.subsumed_indices[reason_idx]] = (*i)[1];
            reason_idx++;
        }
    }
 
    // If LLM didn't return clusters, fall back to deterministic clustering
    if(result.clusters.empty()){
        cerr << "[WARN] LLM returned no clusters, falling back to deterministic clustering." << endl;
 
        // Simple deterministic fallback
        for(int i = 0; i < (int)assertions.size(); i++){
            string a = assertions[i];
            string cat = "other";
 
            // Classify by X(var) target
            regex next_var("X\\(([a-zA-Z_][a-zA-Z0-9_]*)\\)");
            smatch match;
            if(a.find("SPEC AG") != string::npos && a.find("X(") == string::npos)
                cat = "safety_invariants";
            else if(regex_search(a, match, next_var)){
                string target = match[1];
                if(target == "cnt") cat = "count_transitions";
                else if(target == "head") cat = "head_pointer";
                else if(target == "tail") cat = "tail_pointer";
                else if(target[0] == 'q') cat = "memory_slots";
            }
            else if(a.find("X(!empty)") != string::npos || a.find("X(!full)") != string::npos)
                cat = "count_transitions";
            else if(a.find("X(") != string::npos && a.find("q") != string::npos)
                cat = "memory_slots";
 
            result.clusters[cat].push_back(i + 1);
        }
    }
 
    return result;
}
 
/*
    Layer 4 main function: single LLM call for clustering + subsumption, then output
*/
void layer4_subsumption_clustering(){
    cout << "\n[6/6] Running Layer 4: Subsumption & Clustering..." << endl;
 
    vector<string> assertions = read_passed_assertions();
    if(assertions.empty()){
        cerr << "[ERROR] No passed assertions found for Layer 4" << endl;
        return;
    }
    cout << "Layer 4 input: " << assertions.size() << " passed assertions." << endl;
 
    // Single LLM call for both clustering and subsumption
    Layer4Result l4 = run_layer4_llm(assertions);
 
    // Mark subsumed assertions
    vector<bool> subsumed(assertions.size(), false);
    int total_subsumed = 0;
    for(int idx : l4.subsumed_indices){
        int i = idx - 1;  // 1-based -> 0-based
        if(i >= 0 && i < (int)assertions.size() && !subsumed[i]){
            subsumed[i] = true;
            total_subsumed++;
            string reason = l4.subsumption_reasons.count(idx) ? l4.subsumption_reasons[idx] : "no reason given";
            cout << "  SUBSUMED P" << idx << ": " << assertions[i] << endl;
            cout << "    Reason: " << reason << endl;
        }
    }
 
    cout << "\nClusters:" << endl;
    for(auto& [name, indices] : l4.clusters){
        cout << "  " << name << ": " << indices.size() << " assertions" << endl;
    }
    cout << "Subsumption: " << total_subsumed << " removed." << endl;
 
    // Write output files
 
    // File 1: Clustered & deduplicated assertions
    ofstream out_clustered("layer4_clustered_assertions.txt");
 
    // File 2: Detailed report
    ofstream out_report("layer4_report.txt");
 
    out_report << "LAYER 4: SUBSUMPTION & CLUSTERING REPORT\n";
    out_report << "=========================================\n\n";
    out_report << "Input: " << assertions.size() << " passed assertions from Layer 3\n";
    out_report << "Subsumed (removed): " << total_subsumed << "\n";
    out_report << "Remaining: " << (assertions.size() - total_subsumed) << "\n";
    out_report << "LLM calls: 1\n\n";
 
    if(!l4.subsumed_indices.empty()){
        out_report << "--- Subsumption Details ---\n";
        for(int idx : l4.subsumed_indices){
            int i = idx - 1;
            if(i >= 0 && i < (int)assertions.size()){
                string reason = l4.subsumption_reasons.count(idx) ? l4.subsumption_reasons[idx] : "no reason given";
                out_report << "  REMOVED P" << idx << ": " << assertions[i] << "\n";
                out_report << "    Reason: " << reason << "\n";
            }
        }
        out_report << "\n";
    }
 
    int remaining = 0;
    for(auto& [name, indices] : l4.clusters){
        out_report << "--- Cluster: " << name << " ---\n";
        out_clustered << "-- Cluster: " << name << "\n";
 
        for(int idx : indices){
            int i = idx - 1;  // 1-based -> 0-based
            if(i < 0 || i >= (int)assertions.size()) continue;
 
            if(subsumed[i]){
                out_report << "  [SUBSUMED] P" << idx << ": " << assertions[i] << "\n";
            } else {
                out_report << "  [KEPT]     P" << idx << ": " << assertions[i] << "\n";
                out_clustered << assertions[i] << "\n";
                remaining++;
            }
        }
        out_report << "\n";
        out_clustered << "\n";
    }
 
    out_clustered.close();
    out_report.close();
 
    // File 3: Flat list of final assertions (for machine consumption)
    ofstream out_final("layer4_final_assertions.txt");
    for(int i = 0; i < (int)assertions.size(); i++){
        if(!subsumed[i]) out_final << assertions[i] << "\n";
    }
    out_final.close();
 
    cout << "\nLayer 4 Summary: " << remaining << " assertions kept out of "
         << assertions.size() << " (" << total_subsumed << " subsumed)." << endl;
    cout << "Results written to:" << endl;
    cout << "  layer4_clustered_assertions.txt   (clustered, deduplicated)" << endl;
    cout << "  layer4_report.txt                 (detailed report)" << endl;
    cout << "  layer4_final_assertions.txt       (flat list, final)" << endl;
}

string read_failed_with_counterexamples(){
    ifstream file("Failed_formal_verified_assertions.txt");
    if(!file.is_open()) return "";
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
 
/*
    NEW: Generate new assertions using Layer 1 LLM, informed by:
    - The model (specifications.txt)
    - Already-passed assertions (don't regenerate these)
    - Failed assertions + counterexamples (learn from these)
    Returns raw new assertions text from the LLM.
*/
string generate_refinement_assertions(const vector<string>& all_passed,
                                        const string& failed_context,
                                        int iteration){
    string spec = read_specifications();
 
    // Build the passed assertions block
    string passed_block;
    for(int i = 0; i < (int)all_passed.size(); i++){
        passed_block += "  " + to_string(i+1) + ". " + all_passed[i] + "\n";
    }
 
    string prompt = R"(You are a senior hardware formal verification engineer.
You are given:
1. A nuXmv hardware model
2. A set of ALREADY VERIFIED properties (TRUE on the model)
3. A set of FAILED properties with counterexample traces
 
Your task is to generate NEW properties that:
- Cover behaviors NOT yet verified by the existing properties
- Learn from the counterexamples to avoid the same mistakes
- Explore edge cases, corner cases, and interaction scenarios
 
STRICT OUTPUT FORMAT
• Output ONLY nuXmv properties, one per line, ending with semicolon.
• Do NOT include comments, explanations, markdown, or code fences.
 
CRITICAL SYNTAX RULES
• In LTLSPEC: X MUST have parentheses: X(var), X(!var), X(TRUE)
• In SPEC (CTL): use AX not X: SPEC AG (cond -> AX(var) = value);
• All conditions must be INSIDE the temporal scope:
  CORRECT: LTLSPEC G (a & b & c -> X(v) = expr);
  WRONG:   LTLSPEC G (a & b -> X(v) = expr) & (c);
• Inequality operator is != (NOT <>)
• Equality is = (NOT ==)
• Logical AND is & (NOT &&), Logical OR is | (NOT ||)
 
DO NOT GENERATE:
• Any property identical or equivalent to an already-verified one
• Trivial bound checks (SPEC AG cnt <= 4)
• Tautologies (SPEC AG (x -> x))
• Properties assuming environment behavior (LTLSPEC F push)
 
LEARNING FROM COUNTEREXAMPLES:
When you see a failed property with a counterexample, understand WHY it failed:
- If push and pop both happen simultaneously, the property needs guards for that
- If a counterexample shows unexpected state, check what the property assumed wrong
- Generate a CORRECTED version accounting for the counterexample
 
COVERAGE GAPS TO EXPLORE:
- Simultaneous push+pop behavior
- Queue consistency (head/tail relationship to count)
- Data preservation (values written are values read)
- Wrap-around edge cases at all boundary values
- Interactions between different state variables
- Reset/initialization correctness
- Idempotent operations (no-op when full+push or empty+pop)
 
Generate between 5 and 15 NEW properties.
 
MODEL:
)" + spec +
    "\n\nALREADY VERIFIED PROPERTIES (" + to_string(all_passed.size()) + " total):\n" + passed_block;
 
    if(!failed_context.empty()){
        prompt += "\n\nFAILED PROPERTIES WITH COUNTEREXAMPLES:\n" + failed_context;
    }
 
    prompt += "\n\nThis is refinement iteration " + to_string(iteration) + ". Focus on behaviors not yet covered.";
 
    string raw = SpecificationLLM::call_layer1_leader(prompt);
    string text = extractTextGroq(raw);
    string decoded = decodeJsonEscapes(text);
    string cleaned = cleanMarkdown(decoded);
    cleaned = fix_nuxmv_reserved_words(cleaned);
    cleaned = fix_nuxmv_syntax(cleaned);
 
    return cleaned;
}
 
/*
    NEW: Run Layer 2 voting on a vector of assertions.
    Returns only the assertions that pass voting.
*/
vector<string> run_layer2_voting(vector<string>& assertions){
    if(assertions.empty()) return {};
 
    string block = build_assertion_block(assertions);
    string prompt = build_weighted_prompt(block);
    int numLLMs = SpecificationLLM::count_models("layer2Llms");
    vector<vector<VoteResult>> votes(assertions.size());
 
    for(int i = 0; i < numLLMs; i++){
        string raw = SpecificationLLM::call_layer2(prompt, i);
        string text = extractTextGroq(raw);
        string decoded = decodeJsonEscapes(text);
        auto parsed = parse_weighted_votes(decoded);
        for(auto& p : parsed){
            int index = p.first - 1;
            if(index >= 0 && index < (int)assertions.size())
                votes[index].push_back(p.second);
        }
    }
 
    vector<string> passed;
    for(int i = 0; i < (int)assertions.size(); i++){
        if(weighted_decision(votes[i])) passed.push_back(assertions[i]);
    }
    return passed;
}
 
/*
    NEW: Run Layer 3 model checking on a set of assertions.
    Returns PASS and FAIL (with counterexamples) separately.
    Does NOT write to the main output files — self-contained.
*/
struct IterL3Result {
    vector<string> passed;
    vector<pair<string,string>> failed;  // {assertion, counterexample_trace}
};
 
IterL3Result run_layer3_on_assertions(const vector<string>& assertions){
    IterL3Result result;
    if(assertions.empty()) return result;
 
    string model = read_specifications();
    string smv_file = create_combined_smv_model(model, assertions);
    string output = run_nuXmv(smv_file);
    auto nuxmv_results = parse_nuxmv_output(output);
 
    for(int i = 0; i < (int)assertions.size(); i++){
        int idx = i + 1;
        if(nuxmv_results.count(idx) && nuxmv_results[idx].first == "PASS"){
            result.passed.push_back(assertions[i]);
        }
        else if(nuxmv_results.count(idx) && nuxmv_results[idx].first == "FAIL"){
            result.failed.push_back({assertions[i], nuxmv_results[idx].second});
        }
    }
    return result;
}
 
/*
    NEW: Exact string match duplicate check.
*/
bool is_duplicate(const string& assertion, const vector<string>& existing){
    for(auto& e : existing){
        if(e == assertion) return true;
    }
    return false;
}
 
/*
    NEW: Layer 5 main function.
    Iterative loop: generate → vote → model-check → subsume → repeat.
    Max 5 iterations. Stops if no new PASS assertions are found.
*/
void layer5_iterative_refinement(){
    cout << "\n========================================" << endl;
    cout << "Layer 5: Iterative Coverage Refinement" << endl;
    cout << "========================================" << endl;
 
    const int MAX_ITERATIONS = 5;
 
    // Load the master set of passed assertions from Layers 0-4
    vector<string> master_passed = read_passed_assertions();
    if(master_passed.empty()){
        cerr << "[ERROR] No passed assertions found. Run Layers 0-4 first." << endl;
        return;
    }
 
    int initial_count = master_passed.size();
    cout << "Starting with " << initial_count << " verified assertions." << endl;
 
    // Load initial failed context
    string failed_context = read_failed_with_counterexamples();
    if(!failed_context.empty()){
        cout << "Loaded failed assertions with counterexamples for feedback." << endl;
    }
 
    // Iteration report
    ofstream report("iteration_report.txt");
    report << "LAYER 5: ITERATIVE COVERAGE REFINEMENT REPORT\n";
    report << "==============================================\n\n";
    report << "Initial verified assertions: " << initial_count << "\n\n";
 
    int total_new_passed = 0;
    int total_generated = 0;
    int total_voted = 0;
    int total_failed = 0;
 
    for(int iter = 1; iter <= MAX_ITERATIONS; iter++){
        cout << "\n--- Iteration " << iter << "/" << MAX_ITERATIONS << " ---" << endl;
        report << "--- Iteration " << iter << " ---\n";
 
        // Step 1: Generate new assertions from Layer 1 LLM
        cout << "[Iter " << iter << "] Step 1: Generating new assertions (Layer 1)..." << endl;
        string raw_new = generate_refinement_assertions(master_passed, failed_context, iter);
 
        // Parse into individual assertions, deduplicate against master
        vector<string> new_assertions;
        stringstream ss(raw_new);
        string line;
        while(getline(ss, line)){
            string sanitized = sanitize_nuxmv_line(line);
            if(!sanitized.empty() &&
               !is_duplicate(sanitized, master_passed) &&
               !is_duplicate(sanitized, new_assertions)){
                new_assertions.push_back(sanitized);
            }
        }
 
        cout << "[Iter " << iter << "] Generated " << new_assertions.size() << " new unique assertions." << endl;
        report << "  Generated: " << new_assertions.size() << " new unique assertions\n";
        total_generated += new_assertions.size();
 
        // Stop if nothing new
        if(new_assertions.empty()){
            cout << "[Iter " << iter << "] No new assertions generated. Stopping." << endl;
            report << "  STOPPED: No new assertions generated.\n\n";
            break;
        }
 
        // Step 2: Layer 2 voting
        cout << "[Iter " << iter << "] Step 2: Validating assertions (Layer 2 voting)..." << endl;
        vector<string> voted = run_layer2_voting(new_assertions);
        cout << "[Iter " << iter << "] " << voted.size() << "/" << new_assertions.size() << " passed voting." << endl;
        report << "  Voted through: " << voted.size() << "/" << new_assertions.size() << "\n";
        total_voted += voted.size();
 
        if(voted.empty()){
            cout << "[Iter " << iter << "] All assertions rejected by validators. Continuing." << endl;
            report << "  All rejected by validators.\n\n";
            continue;
        }
 
        // Step 3: Layer 3 model checking
        cout << "[Iter " << iter << "] Step 3: Model checking (Layer 3)..." << endl;
        IterL3Result l3 = run_layer3_on_assertions(voted);
        cout << "[Iter " << iter << "] " << l3.passed.size() << " PASS, "
             << l3.failed.size() << " FAIL." << endl;
        report << "  Model check: " << l3.passed.size() << " PASS, "
               << l3.failed.size() << " FAIL\n";
 
        // Update failed context with new failures for next iteration
        if(!l3.failed.empty()){
            failed_context = "";  // Reset to only latest failures (keeps prompt size bounded)
            for(auto& [assertion, trace] : l3.failed){
                failed_context += "FAIL: " + assertion + "\n";
                failed_context += "Counterexample:\n" + trace + "\n\n";
                total_failed++;
            }
        }
 
        // Append new PASS to master
        int new_pass_count = 0;
        for(auto& a : l3.passed){
            if(!is_duplicate(a, master_passed)){
                master_passed.push_back(a);
                new_pass_count++;
            }
        }
 
        total_new_passed += new_pass_count;
        cout << "[Iter " << iter << "] " << new_pass_count << " new unique PASS added to master. "
             << "Master total: " << master_passed.size() << endl;
        report << "  New PASS added: " << new_pass_count << "\n";
        report << "  Master total: " << master_passed.size() << "\n";
 
        // Log new assertions in report
        for(auto& a : l3.passed){
            report << "    [NEW PASS] " << a << "\n";
        }
        for(auto& [a, t] : l3.failed){
            report << "    [NEW FAIL] " << a << "\n";
        }
 
        // Stop if no new PASS
        if(new_pass_count == 0){
            cout << "[Iter " << iter << "] No new PASS assertions. Stopping." << endl;
            report << "  STOPPED: No new PASS assertions.\n\n";
            break;
        }
 
        // Step 4: Run Layer 4 subsumption on updated master to trim redundants
        cout << "[Iter " << iter << "] Step 4: Subsumption (Layer 4)..." << endl;
 
        // Write master to passed file so layer4 can read it
        {
            ofstream pf("passed_formal_verified_assertions.txt");
            for(auto& a : master_passed) pf << a << "\n";
            pf.close();
        }
 
        layer4_subsumption_clustering();
 
        // Re-read the subsumption result as the new master
        // (layer4_final_assertions.txt has the trimmed set)
        {
            ifstream ff("layer4_final_assertions.txt");
            master_passed.clear();
            string fline;
            while(getline(ff, fline)){
                if(fline.empty()) continue;
                size_t s = fline.find_first_not_of(" \t\r\n");
                size_t e = fline.find_last_not_of(" \t\r\n");
                if(s == string::npos) continue;
                string trimmed = fline.substr(s, e - s + 1);
                if(!trimmed.empty()) master_passed.push_back(trimmed);
            }
        }
 
        cout << "[Iter " << iter << "] After subsumption: " << master_passed.size() << " assertions." << endl;
        report << "  After subsumption: " << master_passed.size() << "\n\n";

        if(iter < MAX_ITERATIONS){
            cout << "[Iter " << iter << "] Waiting 60s for API rate limit reset..." << endl;
            sleep(60);
        }
    }
 
    // Write the final master file
    {
        ofstream master_file("master_passed_assertions.txt");
        for(auto& a : master_passed) master_file << a << "\n";
        master_file.close();
    }
 
    // Also update the passed file for Layer 4 final run
    {
        ofstream pf("passed_formal_verified_assertions.txt");
        for(auto& a : master_passed) pf << a << "\n";
        pf.close();
    }
 
    // Final Layer 4 run for clean clustered output
    cout << "\n--- Final subsumption & clustering ---" << endl;
    layer4_subsumption_clustering();
 
    // Summary
    report << "\n=========================================\n";
    report << "FINAL SUMMARY\n";
    report << "=========================================\n";
    report << "Initial assertions:  " << initial_count << "\n";
    report << "Total generated:     " << total_generated << "\n";
    report << "Total voted through: " << total_voted << "\n";
    report << "Total new PASS:      " << total_new_passed << "\n";
    report << "Total new FAIL:      " << total_failed << "\n";
    report << "Final master count:  " << master_passed.size() << "\n";
    report.close();
 
    cout << "\n========================================" << endl;
    cout << "Layer 5 Complete." << endl;
    cout << "Initial: " << initial_count << " → Final: " << master_passed.size()
         << " assertions (+" << (master_passed.size() - initial_count) << " net new)" << endl;
    cout << "Files:" << endl;
    cout << "  master_passed_assertions.txt       (final comprehensive set)" << endl;
    cout << "  iteration_report.txt               (detailed iteration log)" << endl;
    cout << "  layer4_clustered_assertions.txt     (clustered, deduplicated)" << endl;
    cout << "  layer4_final_assertions.txt         (flat list, final)" << endl;
    cout << "========================================" << endl;
}


int main(){

    open_config_file();

    /* This is specification generation part */

    string specifications = generate_specification();
    write_specifications(specifications);

    /* This is layer 1 code */

    generate_assertions_from_leader();
    generate_assertions_from_extenders();

    /* This is layer 2 code */

    verify_assertions_weighted();

    /* This is layer 3 code */

    cout << "[5/6] Running model checking..." << endl;
    layer3_model_checking();

    /* This is layer 4 code */

    layer4_subsumption_clustering();

    /* This is iterative refinement */

    layer5_iterative_refinement();

    cout << "\nPipeline completed successfully.\n";

    return 0;
}