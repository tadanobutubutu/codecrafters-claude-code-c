#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

struct response_buf {
    char *data;
    size_t size;
};

static size_t curl_write_response(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct response_buf *buf = (struct response_buf *)userp;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

int main(int argc, char *argv[]) {
    const char *prompt = NULL;
    if (getopt(argc, argv, "p:") == 'p') prompt = optarg;
    if (!prompt) {
        fprintf(stderr, "error: -p flag is required\n");
        return 1;
    }

    const char *api_key = getenv("OPENROUTER_API_KEY");
    const char *base_url = getenv("OPENROUTER_BASE_URL");
    if (!base_url || !*base_url) base_url = "https://openrouter.ai/api/v1";
    if (!api_key || !*api_key) {
        fprintf(stderr, "OPENROUTER_API_KEY is not set\n");
        return 1;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", base_url);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    cJSON *messages = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
    cJSON_AddItemToArray(messages, user_msg);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int requires_action = 1;
    while (requires_action) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", "anthropic/claude-haiku-4.5");
        cJSON_AddItemToObject(req, "messages", cJSON_Duplicate(messages, 1));

        cJSON *tools = cJSON_AddArrayToObject(req, "tools");

        // Read Tool
        cJSON *read_tool = cJSON_CreateObject();
        cJSON_AddStringToObject(read_tool, "type", "function");
        cJSON *read_func = cJSON_CreateObject();
        cJSON_AddStringToObject(read_func, "name", "Read");
        cJSON_AddStringToObject(read_func, "description", "Read and return the contents of a file");
        cJSON *read_params = cJSON_CreateObject();
        cJSON_AddStringToObject(read_params, "type", "object");
        cJSON *read_props = cJSON_CreateObject();
        cJSON *read_fp = cJSON_CreateObject();
        cJSON_AddStringToObject(read_fp, "type", "string");
        cJSON_AddStringToObject(read_fp, "description", "The path to the file to read");
        cJSON_AddItemToObject(read_props, "file_path", read_fp);
        cJSON_AddItemToObject(read_params, "properties", read_props);
        cJSON *read_req = cJSON_CreateArray();
        cJSON_AddItemToArray(read_req, cJSON_CreateString("file_path"));
        cJSON_AddItemToObject(read_params, "required", read_req);
        cJSON_AddBoolToObject(read_params, "additionalProperties", false);
        cJSON_AddItemToObject(read_func, "parameters", read_params);
        cJSON_AddItemToObject(read_tool, "function", read_func);
        cJSON_AddItemToArray(tools, read_tool);

        // Write Tool
        cJSON *write_tool = cJSON_CreateObject();
        cJSON_AddStringToObject(write_tool, "type", "function");
        cJSON *write_func = cJSON_CreateObject();
        cJSON_AddStringToObject(write_func, "name", "Write");
        cJSON_AddStringToObject(write_func, "description", "Write content to a file");
        cJSON *write_params = cJSON_CreateObject();
        cJSON_AddStringToObject(write_params, "type", "object");
        cJSON *write_props = cJSON_CreateObject();
        
        cJSON *write_fp = cJSON_CreateObject();
        cJSON_AddStringToObject(write_fp, "type", "string");
        cJSON_AddStringToObject(write_fp, "description", "The path of the file to write to");
        cJSON_AddItemToObject(write_props, "file_path", write_fp);
        
        cJSON *write_content = cJSON_CreateObject();
        cJSON_AddStringToObject(write_content, "type", "string");
        cJSON_AddStringToObject(write_content, "description", "The content to write to the file");
        cJSON_AddItemToObject(write_props, "content", write_content);
        
        cJSON_AddItemToObject(write_params, "properties", write_props);
        cJSON *write_req = cJSON_CreateArray();
        cJSON_AddItemToArray(write_req, cJSON_CreateString("file_path"));
        cJSON_AddItemToArray(write_req, cJSON_CreateString("content"));
        cJSON_AddItemToObject(write_params, "required", write_req);
        cJSON_AddBoolToObject(write_params, "additionalProperties", false);
        cJSON_AddItemToObject(write_func, "parameters", write_params);
        cJSON_AddItemToObject(write_tool, "function", write_func);
        cJSON_AddItemToArray(tools, write_tool);

        // Bash Tool
        cJSON *bash_tool = cJSON_CreateObject();
        cJSON_AddStringToObject(bash_tool, "type", "function");
        cJSON *bash_func = cJSON_CreateObject();
        cJSON_AddStringToObject(bash_func, "name", "Bash");
        cJSON_AddStringToObject(bash_func, "description", "Execute a shell command");
        cJSON *bash_params = cJSON_CreateObject();
        cJSON_AddStringToObject(bash_params, "type", "object");
        cJSON *bash_props = cJSON_CreateObject();
        cJSON *bash_cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(bash_cmd, "type", "string");
        cJSON_AddStringToObject(bash_cmd, "description", "The command to execute");
        cJSON_AddItemToObject(bash_props, "command", bash_cmd);
        cJSON_AddItemToObject(bash_params, "properties", bash_props);
        cJSON *bash_req = cJSON_CreateArray();
        cJSON_AddItemToArray(bash_req, cJSON_CreateString("command"));
        cJSON_AddItemToObject(bash_params, "required", bash_req);
        cJSON_AddBoolToObject(bash_params, "additionalProperties", false);
        cJSON_AddItemToObject(bash_func, "parameters", bash_params);
        cJSON_AddItemToObject(bash_tool, "function", bash_func);
        cJSON_AddItemToArray(tools, bash_tool);

        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);

        CURL *curl = curl_easy_init();
        struct response_buf resp = {NULL, 0};
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, auth_header);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(body);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
            free(resp.data);
            break;
        }

        cJSON *json = cJSON_Parse(resp.data);
        free(resp.data);
        if (!json) {
            fprintf(stderr, "Failed to parse response JSON\n");
            break;
        }

        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            fprintf(stderr, "no choices in response\n");
            cJSON_Delete(json);
            break;
        }

        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *assistant_msg = cJSON_GetObjectItem(first, "message");

        // messagesに追加するために複製
        cJSON_AddItemToArray(messages, cJSON_Duplicate(assistant_msg, 1));

        cJSON *tool_calls = cJSON_GetObjectItem(assistant_msg, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            int num_calls = cJSON_GetArraySize(tool_calls);
            for (int i = 0; i < num_calls; i++) {
                cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
                cJSON *id = cJSON_GetObjectItem(tool_call, "id");
                cJSON *func = cJSON_GetObjectItem(tool_call, "function");
                cJSON *func_name = cJSON_GetObjectItem(func, "name");
                cJSON *arguments = cJSON_GetObjectItem(func, "arguments");
                
                const char *args_str = cJSON_GetStringValue(arguments);
                cJSON *args_json = cJSON_Parse(args_str);
                
                char *result_str = NULL;
                
                if (strcmp(cJSON_GetStringValue(func_name), "Read") == 0) {
                    cJSON *fp = cJSON_GetObjectItem(args_json, "file_path");
                    const char *file_path = cJSON_GetStringValue(fp);
                    FILE *f = fopen(file_path, "r");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        long len = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        char *data = malloc(len + 1);
                        size_t read_bytes = fread(data, 1, len, f);
                        data[read_bytes] = '\0';
                        fclose(f);
                        result_str = data;
                    } else {
                        result_str = strdup("Error: Cannot open file");
                    }
                } else if (strcmp(cJSON_GetStringValue(func_name), "Write") == 0) {
                    cJSON *fp = cJSON_GetObjectItem(args_json, "file_path");
                    cJSON *cont = cJSON_GetObjectItem(args_json, "content");
                    const char *file_path = cJSON_GetStringValue(fp);
                    const char *content_val = cJSON_GetStringValue(cont);
                    
                    char *path_dup = strdup(file_path);
                    char *last_slash = strrchr(path_dup, '/');
                    if (last_slash) {
                        *last_slash = '\0';
                        char mkdir_cmd[1024];
                        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", path_dup);
                        int ret = system(mkdir_cmd);
                        (void)ret;
                    }
                    free(path_dup);

                    FILE *f = fopen(file_path, "w");
                    if (f) {
                        fprintf(f, "%s", content_val);
                        fclose(f);
                        char tmp[512];
                        snprintf(tmp, sizeof(tmp), "Successfully wrote to %s", file_path);
                        result_str = strdup(tmp);
                    } else {
                        result_str = strdup("Error: Cannot write to file");
                    }
                } else if (strcmp(cJSON_GetStringValue(func_name), "Bash") == 0) {
                    cJSON *cmd = cJSON_GetObjectItem(args_json, "command");
                    const char *command_val = cJSON_GetStringValue(cmd);
                    
                    char cmd_with_stderr[8192];
                    snprintf(cmd_with_stderr, sizeof(cmd_with_stderr), "%s 2>&1", command_val);
                    FILE *fp_proc = popen(cmd_with_stderr, "r");
                    if (fp_proc) {
                        size_t cap = 4096;
                        size_t len = 0;
                        char *out = malloc(cap);
                        char chunk[256];
                        while (fgets(chunk, sizeof(chunk), fp_proc) != NULL) {
                            size_t chunk_len = strlen(chunk);
                            if (len + chunk_len >= cap) {
                                cap *= 2;
                                out = realloc(out, cap);
                            }
                            strcpy(out + len, chunk);
                            len += chunk_len;
                        }
                        pclose(fp_proc);
                        result_str = out;
                    } else {
                        result_str = strdup("Error: popen failed");
                    }
                }
                
                cJSON_Delete(args_json);
                
                cJSON *tool_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_msg, "role", "tool");
                cJSON_AddStringToObject(tool_msg, "tool_call_id", cJSON_GetStringValue(id));
                cJSON_AddStringToObject(tool_msg, "content", result_str ? result_str : "");
                cJSON_AddItemToArray(messages, tool_msg);
                
                free(result_str);
            }
        } else {
            requires_action = 0;
            cJSON *content_item = cJSON_GetObjectItem(assistant_msg, "content");
            if (cJSON_IsString(content_item)) {
                printf("%s", cJSON_GetStringValue(content_item));
            }
        }
        
        cJSON_Delete(json);
    }

    curl_global_cleanup();
    cJSON_Delete(messages);
    return 0;
}

