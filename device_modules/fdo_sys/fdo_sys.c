/*
 * Copyright 2020 Intel Corporation
 * SPDX-License-Identifier: Apache 2.0
 */


#include "fdo_sys.h"
#include "safe_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "fdo_sys_utils.h"

// CBOR-decoder. Interchangeable with any other CBOR implementation.
static fdor_t *fdor = NULL;
// CBOR-encoder. Interchangeable with any other CBOR implementation.
static fdow_t *fdow = NULL;

// filename that will either be read from or written onto
static char filename[FILE_NAME_LEN];
// service info key that will be used as key on service info map on owner service
static char svi_map_key[SVI_MAP_KEY_LEN];
// position/offset on the file from which data will be read
static size_t file_seek_pos = 0;
// size of the file from which data will be read
static size_t file_sz = 0;
// EOT value whose value is 0 for 'fetch-data'success, and 1 for failure
static int fetch_data_status = 1;
// Number of items in the exec/exec_cb array
// used to perform clean-up on memory allocated for exec/exec_cb instructions
static size_t exec_array_length = 0;
// Number of items in the fetch array
// used to perform clean-up on memory allocated for fetch instructions
static size_t fetch_array_length = 0;
// status_cb isComplete value
static bool status_cb_iscomplete = false;
//  status_cb resultCode value
static int status_cb_resultcode = -1;
// status_cb waitSec value
static uint64_t status_cb_waitsec = -1;
// exec_result to store command execution output
static char exec_result[EXEC_RESULT_LEN];
// local hasMore flag that represents whether the module has data/response to send NOW
// 'true' if there is data to send, 'false' otherwise
static bool hasmore = false;
// local isMore flag that represents whether the module has data/response to send in
// the NEXT messege
// SHOULD be 'true' if there is data to send, 'false' otherwise
// For simplicity, it is 'false' always (also a valid value)
static bool ismore = false;
// the type of operation to perform, generally used to manage responses
static fdoSysModMsg write_type = FDO_SYS_MOD_MSG_NONE;

static bool write_status_cb(char *module_message);
static bool write_data(char *module_message,
	uint8_t *bin_data, size_t bin_len);
static bool write_eot(char *module_message, int status);

int fdo_sys(fdo_sdk_si_type type,
	char *module_message, uint8_t *module_val, size_t *module_val_sz,
	uint16_t *num_module_messages, bool *has_more, bool *is_more, size_t mtu)
{
	int strcmp_filedesc = 1;
	int strcmp_write = 1;
	int strcmp_exec = 1;
	int strcmp_execcb = 1;
	int strcmp_statuscb = 1;
	int strcmp_fetch = 1;
	int result = FDO_SI_INTERNAL_ERROR;
	uint8_t *bin_data = NULL;
	size_t bin_len = 0;
	size_t exec_array_index = 0;
	size_t fetch_array_index = 0;
	size_t status_cb_array_length = 0;
	char **exec_instr = NULL;
	char **fetch_instr = NULL;
	size_t exec_instructions_sz = 0;
	size_t fetch_instructions_sz = 0;
	size_t file_remaining = 0;
	size_t temp_module_val_sz = 0;

	switch (type) {
		case FDO_SI_START:
			// Initialize module's CBOR Reader/Writer objects.
			fdow = ModuleAlloc(sizeof(fdow_t));
			if (!fdow_init(fdow) || !fdo_block_alloc_with_size(&fdow->b, MOD_MAX_BUFF_SIZE)) {
#ifdef DEBUG_LOGS
				printf(
					"Module fdo_sys - FDOW Initialization/Allocation failed!\n");
#endif
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}

			fdor = ModuleAlloc(sizeof(fdor_t));
			if (!fdor_init(fdor) ||
				!fdo_block_alloc_with_size(&fdor->b, MOD_MAX_BUFF_SIZE)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - FDOR Initialization/Allocation failed!\n");
#endif
				goto end;
			}
			result = FDO_SI_SUCCESS;
			goto end;
		case FDO_SI_END:
		case FDO_SI_FAILURE:
			// perform clean-ups as needed
			if (!process_data(FDO_SYS_MOD_MSG_EXIT, NULL, 0, NULL,
				NULL, NULL, NULL, NULL, NULL)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to perform clean-up operations\n");
#endif
				goto end;
			}

			if (fdow) {
				fdow_flush(fdow);
				ModuleFree(fdow);
			}
			if (fdor) {
				fdor_flush(fdor);
				ModuleFree(fdor);
			}
			result = FDO_SI_SUCCESS;
			goto end;
		case FDO_SI_HAS_MORE_DSI:
			// calculate whether there is ServiceInfo to send NOW and update 'has_more'.
			// For testing purposes, set this to true here, and false once first write is done.
			if (!has_more) {
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}

			*has_more = hasmore;
			if (*has_more) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - There is ServiceInfo to send\n");
#endif
			}
			result = FDO_SI_SUCCESS;
			goto end;
		case FDO_SI_IS_MORE_DSI:
			// calculate whether there is ServiceInfo to send in the NEXT iteration
			// and update 'is_more'.
			if (!is_more) {
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}
			// sending either true or false is valid
			// for simplicity, setting this to 'false' always,
			// since managing 'ismore' by looking-ahead can be error-prone
			*is_more = ismore;
			result = FDO_SI_SUCCESS;
			goto end;
		case FDO_SI_GET_DSI_COUNT:
			// return the total number of messages that will be sent in THIS message alone
			// we are always sending 1 message at all times.
			// However, this flag 'case' won't be encountered since this is not invoked
			// by the 'lib/' as of now.
			if (!num_module_messages) {
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}
			*num_module_messages = 1;
			result = FDO_SI_SUCCESS;
			goto end;
		case FDO_SI_GET_DSI:
			// write Device ServiceInfo using 'fdow' by partitioning the messages as per MTU, here.
			if (mtu == 0 || !module_message || !module_val || !module_val_sz) {
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}

			// reset and initialize FDOW's encoder for usage
			fdo_block_reset(&fdow->b);
			if (!fdow_encoder_init(fdow)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to initialize FDOW encoder\n");
#endif
				goto end;
			}

			if (!hasmore || write_type == FDO_SYS_MOD_MSG_NONE) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Invalid state\n");
#endif
				goto end;
			}

			if (write_type == FDO_SYS_MOD_MSG_STATUS_CB) {

				if (!write_status_cb(module_message)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to respond with fdo_sys:status_cb\n");
#endif
					goto end;
				}
				// reset this because module has nothing else left to send
				hasmore = false;
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Responded with fdo_sys:status_cb"
					" [%d, %d, %"PRIu64"]\n",
					status_cb_iscomplete, status_cb_resultcode, status_cb_waitsec);
#endif

			} else if (write_type == FDO_SYS_MOD_MSG_DATA) {

				// if an error occcurs EOT is sent next with failure status code
				fetch_data_status = 1;

				// it's ok to not be able to send data here
				// if anything goes wrong, EOT will be sent now/next, regardless
				result = FDO_SI_SUCCESS;

				// if file size is 0 or has changed since first read or the seek/offset
				// point is more that file size (maybe file is corrupted), finish file transfer
				if (file_sz == 0 || file_sz != get_file_sz(filename) ||
					file_seek_pos > file_sz) {
				// file is empty or doesn't exist
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Empty/Invalid content for fdo_sys:data in %s\n",
						filename);
#endif
					if (!write_eot(module_message, fetch_data_status)) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to respond with fdo_sys:eot\n");
#endif
						goto end;
					}
					result = FDO_SI_SUCCESS;
				} else {

					file_remaining = file_sz - file_seek_pos;
					bin_len = file_remaining > mtu ? mtu : file_remaining;
					bin_data = ModuleAlloc(bin_len * sizeof(uint8_t));
					if (!bin_data) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to alloc for fdo_sys:data buffer\n");
#endif
						goto end;
					}
					if (memset_s(bin_data, bin_len, 0) != 0) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to clear fdo_sys:data buffer\n");
#endif
						goto end;
					}

					if (!read_buffer_from_file_from_pos(filename, bin_data, bin_len, file_seek_pos)) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to read fdo_sys:data content from %s\n",
							filename);
#endif
						if (!write_eot(module_message, fetch_data_status)) {
#ifdef DEBUG_LOGS
							printf("Module fdo_sys - Failed to respond with fdo_sys:eot\n");
#endif
							goto end;
						}
						result = FDO_SI_SUCCESS;
					} else {

						file_seek_pos += bin_len;

						if (!write_data(module_message, bin_data, bin_len)) {
#ifdef DEBUG_LOGS
							printf("Module fdo_sys - Failed to respond with fdo_sys:data\n");
#endif
							goto end;
						}
						hasmore = true;

						// if file is sent completely, then send EOT next
						fetch_data_status = 0;
						if (file_sz == file_seek_pos) {
							write_type = FDO_SYS_MOD_MSG_EOT;
						}
					}
				}

#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Responded with fdo_sys:data containing\n");
#endif
			} else if (write_type == FDO_SYS_MOD_MSG_EOT) {
				if (!write_eot(module_message, fetch_data_status)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to respond with fdo_sys:eot\n");
#endif
					goto end;
				}
				hasmore = false;
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Responded with fdo_sys:eot\n");
#endif
			} else if (write_type == FDO_SYS_MOD_MSG_NONE) {
				// shouldn't reach here, if we do, it might a logical error
				// log and fail
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Invalid module write state\n");
#endif
				goto end;
			}

			if (!fdow_encoded_length(fdow, &temp_module_val_sz)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to get encoded length\n");
#endif
				goto end;
			}
			*module_val_sz = temp_module_val_sz;
			if (memcpy_s(module_val, *module_val_sz,
				fdow->b.block, *module_val_sz) != 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to copy CBOR-encoded module value\n");
#endif
				goto end;
			}
			result = FDO_SI_SUCCESS;
			goto end;
		case FDO_SI_SET_OSI:

		if (!module_message || !module_val || !module_val_sz ||
			*module_val_sz > MOD_MAX_BUFF_SIZE) {
			result = FDO_SI_CONTENT_ERROR;
			goto end;
		}
			// Process the received Owner ServiceInfo contained within 'fdor', here.
		strcmp_s(module_message, FDO_MODULE_MSG_LEN, "filedesc",
					&strcmp_filedesc);
		strcmp_s(module_message, FDO_MODULE_MSG_LEN, "write", &strcmp_write);
		strcmp_s(module_message, FDO_MODULE_MSG_LEN, "exec", &strcmp_exec);
		strcmp_s(module_message, FDO_MODULE_MSG_LEN, "exec_cb", &strcmp_execcb);
		strcmp_s(module_message, FDO_MODULE_MSG_LEN, "status_cb", &strcmp_statuscb);
		strcmp_s(module_message, FDO_MODULE_MSG_LEN, "fetch", &strcmp_fetch);

		if (strcmp_filedesc != 0 && strcmp_exec != 0 &&
					strcmp_write != 0 && strcmp_execcb != 0 &&
					strcmp_statuscb != 0 && strcmp_fetch != 0) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Invalid moduleMessage\n");
#endif
			return FDO_SI_CONTENT_ERROR;
		}

		// reset, copy CBOR data and initialize Parser.
		fdo_block_reset(&fdor->b);
		if (0 != memcpy_s(fdor->b.block, *module_val_sz,
			module_val, *module_val_sz)) {
#ifdef DEBUG_LOGS
			printf("Module fdo_sys - Failed to copy buffer into temporary FDOR\n");
#endif
			goto end;
		}
		fdor->b.block_size = *module_val_sz;

		if (!fdor_parser_init(fdor)) {
#ifdef DEBUG_LOGS
			printf("Module fdo_sys - Failed to init FDOR parser\n");
#endif
			goto end;
		}

		if (strcmp_filedesc == 0) {

			if (!fdor_string_length(fdor, &bin_len)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to read fdo_sys:filedesc length\n");
#endif
				goto end;
			}

			if (bin_len == 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Empty value received for fdo_sys:filedesc\n");
#endif
				// received file name cannot be empty
				return FDO_SI_CONTENT_ERROR;
			}

			bin_data = ModuleAlloc(bin_len * sizeof(uint8_t));
			if (!bin_data) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to alloc for fdo_sys:filedesc\n");
#endif
				goto end;
			}

			if (memset_s(bin_data, bin_len, 0) != 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to clear fdo_sys:filedesc buffer\n");
#endif
				goto end;
			}

			if (!fdor_text_string(fdor, (char *)bin_data, bin_len)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to read fdo_sys:filedesc\n");
#endif
				goto end;
			}

			if (memset_s(filename, sizeof(filename), 0) != 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to clear fdo_sys:filedesc buffer\n");
#endif
				goto end;
			}

			if (0 != strncpy_s(filename, FILE_NAME_LEN,
				(char *)bin_data, bin_len)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to copy fdo:sys:filedesc\n");
#endif
				goto end;
			}

			if (true ==
				delete_old_file((const char *)filename)) {
				result = FDO_SI_SUCCESS;
			}

			goto end;
		} else if (strcmp_write == 0) {

			if (!fdor_string_length(fdor, &bin_len)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to read fdo_sys:write length\n");
#endif
				goto end;
			}

			if (bin_len == 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Empty value received for fdo_sys:write\n");
#endif
				// received file content can be empty for an empty file
				// do not allocate for the same and skip reading the entry
				if (!fdor_next(fdor)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to read fdo_sys:write\n");
#endif
					goto end;
				}
				return FDO_SI_SUCCESS;
			}

			bin_data = ModuleAlloc(bin_len * sizeof(uint8_t));
			if (!bin_data) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to alloc for fdo_sys:write\n");
#endif
				goto end;
			}
			if (memset_s(bin_data, bin_len, 0) != 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to clear fdo_sys:write buffer\n");
#endif
				goto end;
			}

			if (!fdor_byte_string(fdor, bin_data, bin_len)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to read value for fdo_sys:write\n");
#endif
				goto end;
			}

			if (!process_data(FDO_SYS_MOD_MSG_WRITE, bin_data, bin_len, filename,
				NULL, NULL, NULL, NULL, NULL)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to process value for fdo_sys:write\n");
#endif
				goto end;
			}
			result = FDO_SI_SUCCESS;
			goto end;
		} else if (strcmp_exec == 0 || strcmp_execcb == 0) {

			if (!fdor_array_length(fdor, &exec_array_length)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to read fdo_sys:exec/exec_cb array length\n");
#endif
				goto end;
			}

			if (exec_array_length == 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Empty array received for fdo_sys:exec/exec_cb\n");
#endif
				// received exec array cannot be empty
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}

			if (!fdor_start_array(fdor)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to start fdo_sys:exec/exec_cb array\n");
#endif
				goto end;
			}

			// allocate memory for exec_instr
			exec_instr = (char**)ModuleAlloc(sizeof(char*) * (exec_array_length + 1));
			if (!exec_instr) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to alloc for fdo_sys:exec/exec_cb instructions\n");
#endif
				goto end;
			}

			for (exec_array_index = 0; exec_array_index < exec_array_length; exec_array_index++) {
				exec_instr[exec_array_index] =
					(char *)ModuleAlloc(sizeof(char) * MOD_MAX_EXEC_ARG_LEN);
				if (!exec_instr[exec_array_index]) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to alloc for single fdo_sys:exec /exec_cb"
						" instruction\n");
#endif
					goto end;
				}
				if (0 != memset_s(exec_instr[exec_array_index],
					sizeof(sizeof(char) * MOD_MAX_EXEC_ARG_LEN), 0)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys -  Failed to clear single fdo_sys:exec/exec_cb"
					" instruction\n");
#endif
					goto end;
				}
				if (!fdor_string_length(fdor, &exec_instructions_sz) ||
						exec_instructions_sz > MOD_MAX_EXEC_ARG_LEN) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to read fdo_sys:exec/exec_cb text length\n");
#endif
					goto end;
				}
				if (!fdor_text_string(fdor, exec_instr[exec_array_index], exec_instructions_sz)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to read fdo_sys:exec/exec_cb text\n");
#endif
					goto end;
				}

				// 2nd argument is the filename
				if (exec_array_index == 1) {
					if (memset_s(filename, sizeof(filename), 0) != 0) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to clear filename for"
							" fdo_sys:exec/exec_cb\n");
#endif
					goto end;
				}
					if (0 != strncpy_s(filename, FILE_NAME_LEN,
						exec_instr[exec_array_index], exec_instructions_sz)) {
		#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to copy filename for"
							" fdo_sys:exec/exec_cb\n");
		#endif
						goto end;
					}
				}

				// last argument is SVI map key
				if (exec_array_index == exec_array_length-1) {
					if (memset_s(svi_map_key, SVI_MAP_KEY_LEN, 0) != 0) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to clear filename for"
							" fdo_sys:exec/exec_cb\n");
#endif
						goto end;
					}

					if (0 != strncpy_s(svi_map_key, SVI_MAP_KEY_LEN,
						exec_instr[exec_array_index], exec_instructions_sz)) {
		#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to copy svi map key for"
							" fdo_sys:exec/exec_cb\n");
		#endif
						goto end;
					}
				}
			}
			exec_instr[exec_array_index] = NULL;

			if (!fdor_end_array(fdor)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to close fdo_sys:exec/exec_cb array\n");
#endif
				goto end;
			}

			if (strcmp_exec == 0) {
				if (!process_data(FDO_SYS_MOD_MSG_EXEC, NULL, 0, filename,
					exec_instr, NULL, NULL, NULL, NULL)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to process fdo_sys:exec\n");
#endif
					goto end;
				}
			} else if (strcmp_execcb == 0) {
				if (!process_data(FDO_SYS_MOD_MSG_EXEC_CB, NULL, 0, filename,
					exec_instr, &status_cb_iscomplete, &status_cb_resultcode,
					&status_cb_waitsec, exec_result)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to process fdo_sys:exec_cb\n");
#endif
					goto end;
				}

				printf("\n\n RETURNED AFTER EXEC_CB ....... \n\n");
				// respond with initial fdo_sys:status_cb message
				hasmore = true;
				write_type = FDO_SYS_MOD_MSG_STATUS_CB;
			}
			result = FDO_SI_SUCCESS;
			goto end;

		} else if (strcmp_statuscb == 0) {
			if (!fdor_array_length(fdor, &status_cb_array_length)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to process fdo_sys:status_cb array length\n");
#endif
				goto end;
			}
			if (status_cb_array_length != 3) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Invalid number of items in fdo_sys:status_cb\n");
#endif
				goto end;
			}

			if (!fdor_start_array(fdor)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to start fdo_sys:status_cb array\n");
#endif
				goto end;
			}

			if (!fdor_boolean(fdor, &status_cb_iscomplete)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to process fdo_sys:status_cb isComplete\n");
#endif
				goto end;
			}

			if (!fdor_signed_int(fdor, &status_cb_resultcode)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to process fdo_sys:status_cb resultCode\n");
#endif
				goto end;
			}

			if (!fdor_unsigned_int(fdor, &status_cb_waitsec)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to process fdo_sys:status_cb waitSec\n");
#endif
				goto end;
			}

			if (!fdor_end_array(fdor)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to end fdo_sys:status_cb array\n");
#endif
				goto end;
			}

			// if isComplete is true from Owner, then there is going to be no response
			// from the device, Else respond with fdo_sys:status_cb
			if (status_cb_iscomplete) {
				hasmore = false;
				write_type = FDO_SYS_MOD_MSG_NONE;
			} else {
				hasmore = true;
				write_type = FDO_SYS_MOD_MSG_STATUS_CB;
			}

#ifdef DEBUG_LOGS
				printf("Module fdo_sys - fdo_sys:status_cb [%d, %d, %"PRIu64"]\n",
					status_cb_iscomplete, status_cb_resultcode, status_cb_waitsec);
#endif

			if (!process_data(FDO_SYS_MOD_MSG_STATUS_CB, bin_data, bin_len, NULL,
				NULL, &status_cb_iscomplete, &status_cb_resultcode, &status_cb_waitsec, NULL)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to process fdo_sys:status_cb\n");
#endif
				goto end;
			}

			result = FDO_SI_SUCCESS;
			goto end;

		} else if (strcmp_fetch == 0) {
			// ======================== new changes for fetch ==============================

			if (!fdor_array_length(fdor, &fetch_array_length)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to read fdo_sys:fetch array length\n");
#endif
				goto end;
			}

			if (fetch_array_length == 0) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Empty array received for fdo_sys:fetch\n");
#endif
				// received exec array cannot be empty
				result = FDO_SI_CONTENT_ERROR;
				goto end;
			}

			if (!fdor_start_array(fdor)) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to start fdo_sys:fetch array\n");
#endif
				goto end;
			}

			// allocate memory for fetch_instr
			fetch_instr = (char**)ModuleAlloc(sizeof(char*) * (fetch_array_length + 1));
			if (!fetch_instr) {
#ifdef DEBUG_LOGS
				printf("Module fdo_sys - Failed to alloc for fdo_sys:fetch instructions\n");
#endif
				goto end;
			}

			for (fetch_array_index = 0; fetch_array_index < fetch_array_length; fetch_array_index++) {
				fetch_instr[fetch_array_index] =
					(char *)ModuleAlloc(sizeof(char) * MOD_MAX_FETCH_ARG_LEN);
				if (!fetch_instr[fetch_array_index]) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to alloc for single fdo_sys:fetch"
						" instruction\n");
#endif
					goto end;
				}
				if (0 != memset_s(fetch_instr[fetch_array_index],
					sizeof(sizeof(char) * MOD_MAX_FETCH_ARG_LEN), 0)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys -  Failed to clear single fdo_sys:fetch"
					" instruction\n");
#endif
					goto end;
				}
				if (!fdor_string_length(fdor, &fetch_instructions_sz) ||
						fetch_instructions_sz > MOD_MAX_FETCH_ARG_LEN) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to read fdo_sys:fetch text length\n");
#endif
					goto end;
				}
				if (!fdor_text_string(fdor, fetch_instr[fetch_array_index], fetch_instructions_sz)) {
#ifdef DEBUG_LOGS
					printf("Module fdo_sys - Failed to read fdo_sys:fetch text\n");
#endif
					goto end;
				}

				// 1st argument is the filename
				if (fetch_array_index == 0) {
					if (memset_s(filename, sizeof(filename), 0) != 0) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to clear filename for"
							" fdo_sys:fetch\n");
#endif
						goto end;
					}
					if (0 != strncpy_s(filename, FILE_NAME_LEN,
						fetch_instr[fetch_array_index], fetch_instructions_sz)) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to copy filename for"
							" fdo_sys:fetch\n");
#endif
						goto end;
					}
				}

				// 2nd argument is SVI map key
				if (fetch_array_index == 1) {
					if (memset_s(svi_map_key, SVI_MAP_KEY_LEN, 0) != 0) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to clear filename for"
							" fdo_sys:exec/exec_cb\n");
#endif
						goto end;
					}

					if (0 != strncpy_s(svi_map_key, SVI_MAP_KEY_LEN,
						fetch_instr[fetch_array_index], fetch_instructions_sz)) {
#ifdef DEBUG_LOGS
						printf("Module fdo_sys - Failed to copy svi map key for"
							" fdo_sys:exec/exec_cb\n");
#endif
						goto end;
					}
				}
			}

			// set the file size here so that we don't read any more than what we initially saw
			file_sz = get_file_sz(filename);
			hasmore = true;
			// reset the file offset to read a new file
			file_seek_pos = 0;
			write_type = FDO_SYS_MOD_MSG_DATA;
			result = FDO_SI_SUCCESS;
			goto end;
		}

		default:
			result = FDO_SI_FAILURE;
	}

end:
	if (bin_data) {
		ModuleFree(bin_data);
	}
	if (exec_instr && exec_array_length > 0) {
		int exec_counter = exec_array_length - 1;
		while (exec_counter >= 0) {
			ModuleFree(exec_instr[exec_counter]);
			--exec_counter;
		}
		ModuleFree(exec_instr);
		exec_array_length = 0;
	}
	if (fetch_instr && fetch_array_length > 0) {
		int fetch_counter = fetch_array_length - 1;
		while (fetch_counter >= 0) {
			ModuleFree(fetch_instr[fetch_counter]);
			--fetch_counter;
		}
		ModuleFree(fetch_instr);
		fetch_array_length = 0;
	}
	if (result != FDO_SI_SUCCESS) {
		// clean-up state variables/objects
		hasmore = false;
		file_sz = 0;
		file_seek_pos = 0;
		fetch_data_status = 1;
		write_type = FDO_SYS_MOD_MSG_NONE;
	}
	return result;
}

/**
 * Write CBOR-encoded fdo_sys:status_cb content into FDOW.
 */
static bool write_status_cb(char *module_message) {

	printf("Writing status_cb after exec_cb.....\n\n");

	if (!module_message) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Invalid params for fdo_sys:status_cb array\n");
#endif
		return false;
	}

	const char message[] = "status_cb";
	if (memcpy_s(module_message, sizeof(message),
		message, sizeof(message)) != 0) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to copy module message status_cb\n");
#endif
		return false;
	}

	if (!fdow_start_array(fdow, 5)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to start inner fdo_sys:status_cb array\n");
#endif
		return false;
	}

	printf("status complete code: %d\n", status_cb_iscomplete);
	if (!fdow_boolean(fdow, status_cb_iscomplete)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:status_cb isComplete\n");
#endif
		return false;
	}

	printf("status result code: %d\n", status_cb_resultcode);
	if (!fdow_signed_int(fdow, status_cb_resultcode)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:status_cb resultCode\n");
#endif
		return false;
	}

	printf("status wait: %ld\n", status_cb_waitsec);
	if (!fdow_unsigned_int(fdow, status_cb_waitsec)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:status_cb waitSec\n");
#endif
		return false;
	}

	printf("\nexecution result: %s\n", exec_result);
	if (!fdow_text_string(fdow, exec_result, strnlen_s(exec_result, sizeof exec_result))) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:status_cb exec_result\n");
#endif
		return false;
	}

	printf("\nsvi map key=%s\n", svi_map_key);
	if (!fdow_text_string(fdow, svi_map_key, strnlen_s(svi_map_key, sizeof svi_map_key))) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:status_cb svi_map_key\n");
#endif
		return false;
	}

	if (!fdow_end_array(fdow)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to end inner fdo_sys:status_cb array\n");
#endif
		return false;
	}

	return true;
}

/**
 * Write CBOR-encoded fdo_sys:data content into FDOW with given data.
 */
static bool write_data(char *module_message,
	uint8_t *bin_data, size_t bin_len) {

	if (!module_message || !bin_data) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Invalid params for fdo_sys:data\n");
#endif
		return false;
	}

	const char message[] = "data";
	if (memcpy_s(module_message, sizeof(message),
		message, sizeof(message)) != 0) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to copy module message data\n");
#endif
		return false;
	}

	if (!fdow_start_array(fdow, 2)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to start inner fdo_sys:status_cb array\n");
#endif
		return false;
	}

	if (!fdow_byte_string(fdow, bin_data, bin_len)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:data content\n");
#endif
		return false;
	}

	if (!fdow_text_string(fdow, svi_map_key, strnlen_s(svi_map_key, SVI_MAP_KEY_LEN))) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:data svimapkey\n");
#endif
		return false;
	}

	return true;
}

/**
 * Write CBOR-encoded fdo_sys:eot content into FDOW with given status.
 */
static bool write_eot(char *module_message, int status) {

	if (!module_message) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Invalid params for fdo_sys:eot\n");
#endif
		return false;
	}

	const char message[] = "eot";
	if (memcpy_s(module_message, sizeof(message),
		message, sizeof(message)) != 0) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to copy module message eot\n");
#endif
		return false;
	}

	if (!fdow_start_array(fdow, 1)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to start inner array in fdo_sys:eot\n");
#endif
		return false;
	}

	if (!fdow_signed_int(fdow, status)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to write fdo_sys:eot status\n");
#endif
		return false;
	}

	if (!fdow_end_array(fdow)) {
#ifdef DEBUG_LOGS
		printf("Module fdo_sys - Failed to end inner array in fdo_sys:eot\n");
#endif
		return false;
	}

	return true;
}