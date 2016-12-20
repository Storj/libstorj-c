#include "storj.h"

static void queue_next_work(storj_upload_state_t *state);
static int queue_request_bucket_token(storj_upload_state_t *state);
static after_request_token(uv_work_t *work, int status);
static request_token(uv_work_t *work);

static uv_work_t *uv_work_new()
{
    uv_work_t *work = malloc(sizeof(uv_work_t));
    assert(work != NULL);
    return work;
}

//
//     // Encrypt file
//     struct aes256_ctx ctx;
//     uint8_t *file_key_as_hex = calloc(DETERMINISTIC_KEY_SIZE/2 + 1, sizeof(char));
//     str2hex(DETERMINISTIC_KEY_SIZE/2, file_key, file_key_as_hex);
//     aes256_set_decrypt_key(&ctx, file_key_as_hex);
//
//     request_token(work_data);
//
//     // Load original file and tmp file
//     // FILE *original_file;
//     // FILE *encrypted_file;
//     // original_file = fopen(opts->file_path, "r");
//     // encrypted_file = fopen(opts->tmp_path, "w+");
//     //
//     // size_t bytesRead = 0;
//     // int i = 0;
//     // char buffer[512];
//     // memset(buffer, '\0', sizeof(buffer));
//     //
//     // // Read bytes of the original file, encrypt them, and write to the tmp file
//     // if (original_file != NULL) {
//     //   // read up to sizeof(buffer) bytes
//     //   while ((bytesRead = fread(buffer, 1, sizeof(buffer), original_file)) > 0) {
//     //     aes256_encrypt(&ctx, sizeof(buffer), buffer, buffer);
//     //     fputs(buffer, encrypted_file);
//     //     memset(buffer, '\0', sizeof(buffer));
//     //     i++;
//     //   }
//     // }
//     //
//     // // TODO: upload file
//     //
//     // fclose(original_file);
//     // fclose(encrypted_file);
//     //
//     // unlink(encrypted_file);
//     free(file_id);
//     free(file_id_buff);
//
//
// }
//
//
//

static after_request_token(uv_work_t *work, int status)
{
    token_request_token_t *req = work->data;

    req->upload_state->token_request_count += 1;

    printf("token: %s\n", req->token);
    printf("status_code: %d\n", req->status_code);
    printf("bucket_op: %s\n", req->bucket_op);
    printf("bucket_id: %s\n", req->bucket_id);

    // Check if we got a statu
    if (req->status_code == 201 && req->token) {
        req->upload_state->requesting_token = false;
        req->upload_state->token = req->token;
    } else if (req->upload_state->token_request_count == 6) {
        req->upload_state->error_code = 1;
    } else {
        queue_request_bucket_token(req->upload_state);
    }

    queue_next_work(req->upload_state);

    free(req);
    free(work);
}

static request_token(uv_work_t *work)
{
    token_request_token_t *req = work->data;

    int path_len = strlen(req->bucket_id) + 17;
    char *path = calloc(path_len + 1, sizeof(char));
    sprintf(path, "%s%s%s%c", "/buckets/", req->bucket_id, "/tokens", '\0');

    struct json_object *body = json_object_new_object();
    json_object *op_string = json_object_new_string(req->bucket_op);
    json_object_object_add(body, "operation", op_string);

    int *status_code;
    struct json_object *response = fetch_json(req->options,
                                              "POST",
                                              path,
                                              body,
                                              true,
                                              NULL,
                                              &status_code);

    struct json_object *token_value;
    if (!json_object_object_get_ex(response, "token", &token_value)) {
        //TODO Log that we failed to get a token
    }

    if (!json_object_is_type(token_value, json_type_string) == 1) {
        // TODO error
    }

    req->token = (char *)json_object_get_string(token_value);
    req->status_code = status_code;

    free(path);
    json_object_put(response);
    json_object_put(body);
}

static int queue_request_bucket_token(storj_upload_state_t *state)
{
    uv_work_t *work = uv_work_new();

    token_request_token_t *req = malloc(sizeof(token_request_token_t));
    assert(req != NULL);

    req->options = state->env->bridge_options;
    req->bucket_id = state->bucket_id;
    req->bucket_op = BUCKET_OP[BUCKET_PUSH];
    req->upload_state = state;
    work->data = req;

    int status = uv_queue_work(state->env->loop, (uv_work_t*) work,
                               request_token, after_request_token);

    // TODO check status
    state->requesting_token = true;

    return status;
}

static void queue_next_work(storj_upload_state_t *state)
{
    // report any errors
    if (state->error_code != 0) {
        // TODO make sure that finished_cb is not called multiple times
        state->final_callback_called = true;
        state->finished_cb(state->error_code);
        free(state);
        return;
    }

    // queue_write_next_shard(state);
    //
    // // report progress of upload
    // if (state->total_bytes > 0 && state->uploaded_bytes > 0) {
    //     state->progress_cb(state->uploaded_bytes / state->total_bytes);
    // }

    // report upload complete
    if (state->completed_shards == state->total_shards) {
        state->final_callback_called = true;
        state->finished_cb(0);
        free(state);
        return;
    }

    if (!state->token && state->requesting_token != true) {
        queue_request_bucket_token(state);
    }

    // if (state->token && !state->pointers_completed) {
    //     queue_request_pointers(state);
    // }
    //
    // queue_request_shards(state);
}

static void begin_work_queue(uv_work_t *work)
{
    storj_upload_state_t *state = work->data;

    queue_next_work(state);

    free(work);
}


static void prepare_upload_state(uv_work_t *work)
{
    storj_upload_state_t *state = work->data;

    if (strchr(PATH_SEPARATOR, state->file_path)) {
        state->file_name = strrchr(state->file_path, PATH_SEPARATOR);
        // Remove '/' from the front if exists by pushing the pointer up
        if (state->file_name[0] == PATH_SEPARATOR) state->file_name++;
    } else {
        state->file_name = state->file_path;
    }

    // Get the file size
    state->file_size = check_file(state->env, state->file_path); // Expect to be up to 10tb
    if (state->file_size < 1) {
        state->error_code = 1; // TODO: Make actual error Codes.
        return;
    }

    // Set Shard calculations
    state->shard_size = determine_shard_size(state, 0);
    state->total_shards = ceil((double)state->file_size / state->shard_size);

    // Generate encryption key && Calculate deterministic file id
    char *file_id = calloc(FILE_ID_SIZE + 1, sizeof(char));
    char *file_key = calloc(DETERMINISTIC_KEY_SIZE + 1, sizeof(char));

    calculate_file_id(state->bucket_id, state->file_name, &file_id);
    memcpy(state->file_id, file_id, FILE_ID_SIZE);
    state->file_id[FILE_ID_SIZE] = '\0';
    generate_file_key(state->mnemonic, state->bucket_id, state->file_id, &file_key);
    memcpy(state->file_key, file_key, DETERMINISTIC_KEY_SIZE);
    state->file_key[DETERMINISTIC_KEY_SIZE] = '\0';

    // Set tmp file
    int tmp_len = strlen(state->file_path) + strlen(".crypt");
    char *tmp_path = calloc(tmp_len + 1, sizeof(char));
    strcpy(tmp_path, state->file_path);
    strcat(tmp_path, ".crypt");
    state->tmp_path = tmp_path;

    free(tmp_path);
    free(file_id);
    free(file_key);
}

int storj_bridge_store_file(storj_env_t *env,
                            storj_upload_opts_t *opts,
                            storj_progress_cb progress_cb,
                            storj_finished_upload_cb finished_cb)
{
    if (opts->file_concurrency < 1) {
        printf("\nFile Concurrency (%i) can't be less than 1", opts->file_concurrency);
        return ERROR;
    } else if (!opts->file_concurrency) {
        opts->file_concurrency = 1;
    }

    if (opts->shard_concurrency < 1) {
        printf("\nShard Concurrency (%i) can't be less than 1", opts->shard_concurrency);
        return ERROR;
    } else if (!opts->shard_concurrency) {
        opts->shard_concurrency = 3;
    }

    // setup upload state
    storj_upload_state_t *state = malloc(sizeof(storj_upload_state_t));
    state->file_concurrency = opts->file_concurrency;
    state->shard_concurrency = opts->shard_concurrency;
    state->uploaded_bytes = 0;
    state->env = env;
    state->file_path = opts->file_path;
    state->bucket_id = opts->bucket_id;
    state->progress_cb = progress_cb;
    state->finished_cb = finished_cb;
    state->total_shards = 0;
    state->completed_shards = 0;
    state->writing = false;
    state->token = NULL;
    state->requesting_token = false;
    state->final_callback_called = false;
    state->mnemonic = opts->mnemonic;
    state->error_code = 0;
    state->token_request_count = 0;

    uv_work_t *work = uv_work_new();
    work->data = state;

    return uv_queue_work(env->loop, (uv_work_t*) work, prepare_upload_state, begin_work_queue);
}
