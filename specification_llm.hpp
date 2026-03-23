#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>
#include <unistd.h>

class SpecificationLLM {

public:

static std::string call_specsllm(const std::string& prompt){
    return callFromLayer("specsllm", prompt, 0);
}

static std::string call_layer1_leader(const std::string& prompt){
    return callFromLayer("layer1Llms", prompt, 0);
}

static std::string call_layer1_extender(const std::string& prompt, int extenderIndex){
    return callFromLayer("layer1Llms", prompt, extenderIndex);
}

static std::string call_layer2(const std::string& prompt, int extenderIndex){
    return callFromLayer("layer2Llms", prompt, extenderIndex);
}

// ADDED: Layer 4 LLM call for subsumption & clustering
static std::string call_layer4(const std::string& prompt, int modelIndex){
    return callFromLayer("layer4Llms", prompt, modelIndex);
}

static int count_models(const std::string& layer){
    std::ifstream file("config.json");
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string config = buffer.str();
    size_t layerPos = config.find(layer);
    if(layerPos == std::string::npos) return 0;

    size_t nextLayer = config.find("]", layerPos);
    if(nextLayer == std::string::npos) return 0;

    int count = 0;
    size_t pos = layerPos;
    while(true){
        pos = config.find("modelId", pos + 1);
        if(pos == std::string::npos) break;
        if(pos > nextLayer) break;
        count++;
    }
    return count;
}

private:

static std::string callFromLayer(const std::string& layer, const std::string& prompt, int modelIndex){

    // Rate limit pacer: every 3 calls, sleep 30s
    static int call_counter = 0;
    call_counter++;
    if(call_counter > 1 && call_counter % 3 == 0){
        std::cerr << "[API] Rate limit pacer: " << call_counter
                  << " calls made, sleeping 30s..." << std::endl;
        sleep(30);
    }

    // read config.json
    std::ifstream file("config.json");
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string config = buffer.str();

    // locate layer in config
    size_t layerPos = config.find(layer);

    if(layerPos == std::string::npos){
        std::cerr << "Layer not found in config\n";
        return "";
    }

    size_t modelPos = layerPos;

    for(int i=0;i<=modelIndex;i++){
        modelPos = config.find("modelId", modelPos + 1);
    }

    size_t q1 = config.find("\"", modelPos + 9);
    size_t q2 = config.find("\"", q1 + 1);

    std::string modelId = config.substr(q1 + 1, q2 - q1 - 1);

    size_t keyPos = layerPos;

    for(int i=0;i<=modelIndex;i++){
        keyPos = config.find("apiKey", keyPos + 1);
    }

    q1 = config.find("\"", keyPos + 7);
    q2 = config.find("\"", q1 + 1);

    std::string apiKey = config.substr(q1 + 1, q2 - q1 - 1);
    std::string command;

    if(layer == "specsllm") command = command_specsllm(modelId, apiKey, prompt);
    else if(layer == "layer1Llms") command = command_layer1(modelId, apiKey, prompt);
    else if(layer == "layer2Llms") command = command_layer1(modelId, apiKey, prompt);
    else if(layer == "layer4Llms") command = command_layer1(modelId, apiKey, prompt);  // ADDED

    // Retry loop: up to 3 attempts on rate limit errors
    for(int attempt = 1; attempt <= 3; attempt++){

        std::cerr << "[API] Calling " << layer << "[" << modelIndex << "] model=" << modelId;
        if(attempt > 1) std::cerr << " (retry " << attempt << ")";
        std::cerr << " ..." << std::endl;

        std::string result;
        FILE* pipe = popen(command.c_str(), "r");

        if(!pipe){
            std::cerr << "[API] ERROR: Failed to open pipe for curl" << std::endl;
            return "";
        }

        char buffer2[256];
        while(fgets(buffer2, sizeof(buffer2), pipe)){
            result += buffer2;
        }
        pclose(pipe);

        if(result.empty()){
            std::cerr << "[API] WARNING: Empty response from " << modelId << std::endl;
            return "";
        }

        // Check for rate limit error
        if(result.find("rate_limit") != std::string::npos ||
           result.find("Rate limit") != std::string::npos){

            if(attempt < 3){
                int wait = 30;
                size_t wp = result.find("try again in ");
                if(wp != std::string::npos){
                    try { wait = (int)(std::stof(result.substr(wp + 13, 10)) + 1); }
                    catch(...) { wait = 30; }
                }
                std::cerr << "[API] Rate limit hit. Waiting " << wait << "s..." << std::endl;
                sleep(wait);
                continue;
            }
            std::cerr << "[API] Rate limit hit 3 times. Giving up." << std::endl;
            return result;
        }

        std::cerr << "[API] Got " << result.size() << " bytes from " << modelId << std::endl;
        return result;
    }

    return "";
}

static std::string command_specsllm(const std::string& modelId, const std::string& apiKey, const std::string& prompt){
    std::string command =
        "curl -s --connect-timeout 30 --max-time 120 https://generativelanguage.googleapis.com/v1beta/models/"
        + modelId +
        ":generateContent?key=" + apiKey +
        " -H \"Content-Type: application/json\" "
        "-d '{\"contents\":[{\"parts\":[{\"text\":\"" + escapeJson(prompt) + "\"}]}]}'";

    return command;
}

static std::string escapeJson(const std::string& input){

    std::string output;

    for(char c : input){

        if(c == '\"') output += "\\\"";
        else if(c == '\n') output += "\\n";
        else if(c == '\r') output += "\\r";
        else if(c == '\t') output += "\\t";
        else if(c == '\\') output += "\\\\";
        else output += c;
    }

    return output;
}

static std::string command_layer1(const std::string& modelId, const std::string& apiKey, const std::string& prompt){
    std::string escapedPrompt = escapeJson(prompt);

    std::string command =
    "curl -s --connect-timeout 30 --max-time 120 https://api.groq.com/openai/v1/chat/completions "
    "-H \"Content-Type: application/json\" "
    "-H \"Authorization: Bearer " + apiKey + "\" "
    "-d '{"
    "\"model\":\"" + modelId + "\","
    "\"messages\":[{\"role\":\"user\",\"content\":\"" + escapedPrompt + "\"}]"
    "}'";

    return command;
}

};