/*
 * COPYRIGHT (c) International Business Machines Corp. 2017
 *
 * This program is provided under the terms of the Common Public License,
 * version 1.0 (CPL-1.0). Any use, reproduction or distribution for this
 * software constitutes recipient's acceptance of CPL-1.0 terms which can be
 * found in the file LICENSE file or at
 * https://opensource.org/licenses/cpl1.0.php
 */

/* Management tool for EP11 sessions.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <dlfcn.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <regex.h>
#include <dirent.h>
#include <libgen.h>
#include <errno.h>

#define OCK_NO_EP11_DEFINES
#include "../../include/pkcs11types.h"
#include "../../lib/common/p11util.h"
#include "../../lib/ep11_stdll/ep11_func.h"
#include "pin_prompt.h"

#define EP11SHAREDLIB_NAME "OCK_EP11_LIBRARY"
#define EP11SHAREDLIB_V4 "libep11.so.4"
#define EP11SHAREDLIB_V3 "libep11.so.3"
#define EP11SHAREDLIB_V2 "libep11.so.2"
#define EP11SHAREDLIB_V1 "libep11.so.1"
#define EP11SHAREDLIB "libep11.so"
#define PKCS11_MAX_PIN_LEN 128

#define CKH_IBM_EP11_SESSION     CKH_VENDOR_DEFINED + 1
#define CKH_IBM_EP11_VHSMPIN     CKH_VENDOR_DEFINED + 2
#define CKA_HIDDEN               CKA_VENDOR_DEFINED + 0x01000000

#define UNUSED(var)            ((void)(var))

#define SHA256_HASH_SIZE        32
#define EP11_SESSION_ID_SIZE    16
#define SYSFS_DEVICES_AP        "/sys/devices/ap/"
#define REGEX_CARD_PATTERN      "card[0-9a-fA-F]+"
#define REGEX_SUB_CARD_PATTERN  "[0-9a-fA-F]+\\.[0-9a-fA-F]+"
#define MASK_EP11               0x04000000

typedef struct {
    short format;
    short length;
    short apqns[512];
} __attribute__ ((packed)) ep11_target_t;

typedef CK_RV (*handler_t) (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE obj,
                            CK_BYTE *pin_blob, CK_ULONG pin_blob_size,
                            CK_BYTE *session_id, CK_ULONG session_id_len,
                            ep11_target_t *ep11_targets,
                            pid_t pid, CK_DATE *date);
typedef CK_RV (*adapter_handler_t) (uint_32 adapter, uint_32 domain,
                                    void *handler_data);

CK_FUNCTION_LIST *funcs;
m_init_t dll_m_init;
m_Logout_t dll_m_Logout;
m_add_module_t dll_m_add_module;
m_rm_module_t dll_m_rm_module;
m_get_xcp_info_t dll_m_get_xcp_info;
CK_SLOT_ID SLOT_ID = -1;
int action = 0;
int force = 0;
time_t filter_date = -1;
pid_t filter_pid = 0;
char filter_sess_id[EP11_SESSION_ID_SIZE];
int filter_sess_id_set = 0;
unsigned long count = 0;
CK_RV error = CKR_OK;
CK_VERSION lib_version;

#define ACTION_SHOW     1
#define ACTION_LOGOUT   2
#define ACTION_VHSMPIN  3
#define ACTION_STATUS   4

static int get_user_pin(CK_BYTE *dest)
{
    int ret = -1;
    const char *userpin;
    char *buf_user = NULL;

    userpin = pin_prompt(&buf_user, "Enter the USER PIN: ");
    if (!userpin) {
        fprintf(stderr, "Could not get USER PIN.\n");
        goto out;
    }

    if (strlen(userpin) > PKCS11_MAX_PIN_LEN) {
        fprintf(stderr, "The USER PIN must be less than %d chars in length.\n",
                (int) PKCS11_MAX_PIN_LEN);
        goto out;
    }

    memcpy(dest, userpin, strlen(userpin) + 1);
    ret = 0;
out:
    pin_free(&buf_user);
    return ret;
}

static int get_vhsm_pin(CK_BYTE *dest)
{
    int ret = -1;
    const char *vhsmpin = NULL;
    char *buf_vhsm = NULL;
    size_t vhsmpinlen;

    vhsmpin = pin_prompt_new(&buf_vhsm,
                             "Enter the new VHSM PIN: ",
                             "Re-enter the new VHSM PIN: ");
    if (!vhsmpin) {
        fprintf(stderr, "Could not get VHSM PIN.\n");
        goto out;
    }
    vhsmpinlen = strlen(vhsmpin);

    if (vhsmpinlen < XCP_MIN_PINBYTES) {
        fprintf(stderr, "The VHSM PIN must be at least %d chars in length.\n",
                (int) XCP_MIN_PINBYTES);
        goto out;
    }
    if (vhsmpinlen > XCP_MAX_PINBYTES) {
        fprintf(stderr, "The VHSM PIN must be less than %d chars in length.\n",
                (int) XCP_MAX_PINBYTES);
        goto out;
    }

    memcpy(dest, vhsmpin, vhsmpinlen + 1);
    ret = 0;
out:
    pin_free(&buf_vhsm);
    return ret;
}

static int do_GetFunctionList(void)
{
    CK_RV rc;
    CK_RV (*func_list)(CK_FUNCTION_LIST_PTR_PTR ppFunctionList) = NULL;
    void *d;
    char *evar;
    char *evar_default = "libopencryptoki.so";

    evar = secure_getenv("PKCSLIB");
    if (evar == NULL)
        evar = evar_default;

    d = dlopen(evar, RTLD_NOW);
    if (d == NULL)
        return 0;

    *(void **)(&func_list) = dlsym(d, "C_GetFunctionList");
    if (func_list == NULL)
        return 0;

    rc = func_list(&funcs);

    if (rc != CKR_OK)
        return 0;

    return 1;
}

int is_ep11_token(CK_SLOT_ID slot_id)
{
    CK_RV rc;
    CK_TOKEN_INFO tokinfo;

    rc = funcs->C_GetTokenInfo(slot_id, &tokinfo);
    if (rc != CKR_OK)
        return FALSE;

    return strstr((const char *) tokinfo.model, "EP11") != NULL;
}

static void usage(char *fct)
{
    printf("usage:  %s show|logout|vhsmpin|status [-date <yyyy/mm/dd>] [-pid <pid>] "
           "[-id <sess-id>] [-slot <num>] [-force] [-h]\n\n", fct);
    return;
}

static int do_ParseArgs(int argc, char **argv)
{
    int i, k;
    struct tm tm;
    char *p;
    unsigned int v;

    if (argc <= 1) {
        printf("No Arguments given. For help use the '--help' or '-h' "
               "option.\n");
        return -1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    } else if (strcmp(argv[1], "show") == 0) {
        action = ACTION_SHOW;
    } else if (strcmp(argv[1], "logout") == 0) {
        action = ACTION_LOGOUT;
    } else if (strcmp(argv[1], "vhsmpin") == 0) {
        action = ACTION_VHSMPIN;
    } else if (strcmp(argv[1], "status") == 0) {
        action = ACTION_STATUS;
    } else {
        printf("Unknown Action given. For help use the '--help' or '-h' "
               "option.\n");
        return -1;
    }

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (action == ACTION_STATUS) {
            printf("Argument '%s' not accepted for 'status' command\n",
                   argv[i]);
            return -1;
        } else if (strcmp(argv[i], "-slot") == 0) {
            if (argc <= i + 1 || !isdigit(*argv[i + 1])) {
                printf("Slot parameter is not numeric!\n");
                return -1;
            }
            SLOT_ID = (int) strtol(argv[i + 1], NULL, 0);
            i++;
        } else if (strcmp(argv[i], "-force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "-date") == 0) {
            if (argc <= i + 1 || strlen(argv[i + 1]) == 0) {
                printf("Date parameter is not valid!\n");
                return -1;
            }
            memset(&tm, 0, sizeof(tm));
            p = strptime(argv[i + 1], "%Y/%m/%d", &tm);
            if (p == NULL || *p != '\0') {
                printf("Date parameter is not valid!\n");
                return -1;
            }
            filter_date = mktime(&tm);
            if (filter_date == -1) {
                printf("Date parameter is not valid!\n");
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], "-pid") == 0) {
            if (argc <= i + 1 || !isdigit(*argv[i + 1])) {
                printf("Pid parameter is not numeric!\n");
                return -1;
            }
            filter_pid = (pid_t) strtol(argv[i + 1], NULL, 0);
            i++;
        } else if (strcmp(argv[i], "-id") == 0) {
            if (argc <= i + 1
                || strlen(argv[i + 1]) != EP11_SESSION_ID_SIZE * 2) {
                printf("Id parameter is not valid!\n");
                return -1;
            }
            p = argv[i + 1];
            for (k = 0; k < EP11_SESSION_ID_SIZE; k++, p += 2) {
                if (sscanf(p, "%02X", &v) != 1) {
                    printf("Id parameter is not valid!\n");
                    return -1;
                }
                filter_sess_id[k] = v;
            }
            filter_sess_id_set = 1;
            i++;
        } else {
            printf("Invalid argument passed as option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    if (action != ACTION_STATUS && SLOT_ID == (CK_SLOT_ID)(-1)) {
        printf("Slot-ID not set!\n");
        return -1;
    }

    return 1;
}

static int is_process_running(pid_t pid)
{
    char fbuf[800];
    int fd;

    sprintf(fbuf, "/proc/%d/stat", pid);
    if ((fd = open(fbuf, O_RDONLY, 0)) == -1)
        return FALSE;

    close(fd);

    return TRUE;
}

static CK_RV get_ep11_library_version(CK_VERSION *lib_version)
{
    unsigned int host_version;
    CK_ULONG version_len = sizeof(host_version);
    CK_RV rc;

    rc = dll_m_get_xcp_info(&host_version, &version_len,
                            CK_IBM_XCPHQ_VERSION, 0, 0);
    if (rc != CKR_OK) {
        fprintf(stderr, "dll_m_get_xcp_info (HOST) failed: rc=0x%lx\n", rc);
        return rc;
    }
    lib_version->major = (host_version & 0x00FF0000) >> 16;
    lib_version->minor = host_version & 0x000000FF0000;
    /*
     * EP11 host library < v2.0 returns an invalid version (i.e. 0x100). This
     * can safely be treated as version 1.0
     */
    if (lib_version->major == 0) {
        lib_version->major = 1;
        lib_version->minor = 0;
    }

    return CKR_OK;
}

static CK_RV logout_handler(uint_32 adapter, uint_32 domain, void *handler_data)
{
    ep11_target_t target_list;
    struct XCP_Module module;
    target_t target = XCP_TGT_INIT;
    CK_RV rc;

    if (dll_m_add_module != NULL) {
        memset(&module, 0, sizeof(module));
        module.version = lib_version.major >= 3 ? XCP_MOD_VERSION_2
                                                : XCP_MOD_VERSION_1;
        module.flags = XCP_MFL_MODULE;
        module.module_nr = adapter;
        XCPTGTMASK_SET_DOM(module.domainmask, domain);
        rc = dll_m_add_module(&module, &target);
        if (rc != 0)
            return CKR_FUNCTION_FAILED;
    } else {
        /* Fall back to old target handling */
        memset(&target_list, 0, sizeof(ep11_target_t));
        target_list.length = 1;
        target_list.apqns[0] = adapter;
        target_list.apqns[1] = domain;
        target = (target_t)&target_list;
    }

    rc = dll_m_Logout(handler_data, XCP_PINBLOB_BYTES, target);
    if (rc != CKR_OK && rc != CKR_SESSION_CLOSED) {
        fprintf(stderr,
                "WARNING: Logout failed for adapter %02X.%04X: 0x%lx [%s]\n",
                adapter, domain, rc, p11_get_ckr(rc));
        error = rc;
    }

    if (dll_m_rm_module != NULL)
        dll_m_rm_module(&module, target);

    return CKR_OK;
}

static CK_RV file_fgets(const char *fname, char *buf, size_t buflen)
{
    FILE *fp;
    char *end;
    CK_RV rc = CKR_OK;

    buf[0] = '\0';

    fp = fopen(fname, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open file '%s'\n", fname);
        return CKR_FUNCTION_FAILED;
    }
    if (fgets(buf, buflen, fp) == NULL) {
        fprintf(stderr, "Failed to read from file '%s'\n", fname);
        rc = CKR_FUNCTION_FAILED;
        goto out_fclose;
    }

    end = memchr(buf, '\n', buflen);
    if (end)
        *end = 0;
    else
        buf[buflen - 1] = 0;

    if (strlen(buf) == 0) {
        rc = CKR_FUNCTION_FAILED;
        goto out_fclose;
    }

out_fclose:
    fclose(fp);

    return rc;
}

static CK_RV is_card_ep11_and_online(const char *name)
{
    char fname[290];
    char buf[250];
    CK_RV rc;
    unsigned long val;

#ifdef EP11_HSMSIM
    return CKR_OK;
#endif

    sprintf(fname, "%s%s/online", SYSFS_DEVICES_AP, name);
    rc = file_fgets(fname, buf, sizeof(buf));
    if (rc != CKR_OK)
        return rc;
    if (strcmp(buf, "1") != 0)
        return CKR_FUNCTION_FAILED;

    sprintf(fname, "%s%s/config", SYSFS_DEVICES_AP, name);
    rc = file_fgets(fname, buf, sizeof(buf));
    if (rc == CKR_OK && strcmp(buf, "1") != 0)
        return CKR_FUNCTION_FAILED;

    sprintf(fname, "%s%s/chkstop", SYSFS_DEVICES_AP, name);
    rc = file_fgets(fname, buf, sizeof(buf));
    if (rc == CKR_OK && strcmp(buf, "0") != 0)
        return CKR_FUNCTION_FAILED;

    sprintf(fname, "%s%s/ap_functions", SYSFS_DEVICES_AP, name);
    rc = file_fgets(fname, buf, sizeof(buf));
    if (rc != CKR_OK)
        return rc;
    if (sscanf(buf, "%lx", &val) != 1)
        val = 0x00000000;
    if ((val & MASK_EP11) == 0)
        return CKR_FUNCTION_FAILED;

    return CKR_OK;
}

static CK_RV scan_for_card_domains(const char *name, adapter_handler_t handler,
                                   void *handler_data)
{
    char fname[290];
    regex_t reg_buf;
    regmatch_t pmatch[1];
    DIR *d;
    struct dirent *de;
    char *tok;
    uint_32 adapter, domain;

#ifdef EP11_HSMSIM
    return handler(0, 0, handler_data);
#endif

    if (regcomp(&reg_buf, REGEX_SUB_CARD_PATTERN, REG_EXTENDED) != 0) {
        fprintf(stderr, "Failed to compile regular expression '%s'\n",
                REGEX_SUB_CARD_PATTERN);
        return CKR_FUNCTION_FAILED;
    }

    sprintf(fname, "%s%s/", SYSFS_DEVICES_AP, name);
    d = opendir(fname);
    if (d == NULL) {
        fprintf(stderr, "Directory %s is not available\n", fname);
        regfree(&reg_buf);
        // ignore this error, card may have been removed in the meantime
        return CKR_OK;
    }

    while ((de = readdir(d)) != NULL) {
        if (regexec(&reg_buf, de->d_name, (size_t) 1, pmatch, 0) == 0) {
            tok = strtok(de->d_name, ".");
            if (tok == NULL)
                continue;
            if (sscanf(tok, "%x", &adapter) != 1)
                continue;

            tok = strtok(NULL, ",");
            if (tok == NULL)
                continue;
            if (sscanf(tok, "%x", &domain) != 1)
                continue;

            if (handler(adapter, domain, handler_data) != CKR_OK)
                break;
        }
    }

    closedir(d);
    regfree(&reg_buf);

    return CKR_OK;
}

/*
 * Iterate over all cards in the sysfs directorys /sys/device/ap/cardxxx
 * and check if the card is online. Calls the handler function for all
 * online EP11 cards.
 */
static CK_RV scan_for_ep11_cards(adapter_handler_t handler, void *handler_data)
{
    DIR *d;
    struct dirent *de;
    regex_t reg_buf;
    regmatch_t pmatch[1];

    if (handler == NULL)
        return CKR_ARGUMENTS_BAD;

#ifdef EP11_HSMSIM
    return handler(0, 0, handler_data);
#endif

    if (regcomp(&reg_buf, REGEX_CARD_PATTERN, REG_EXTENDED) != 0) {
        fprintf(stderr, "Failed to compile regular expression '%s'\n",
                REGEX_CARD_PATTERN);
        return CKR_FUNCTION_FAILED;
    }

    d = opendir(SYSFS_DEVICES_AP);
    if (d == NULL) {
        fprintf(stderr, "Directory %s is not available\n", SYSFS_DEVICES_AP);
        regfree(&reg_buf);
        return CKR_FUNCTION_FAILED;
    }

    while ((de = readdir(d)) != NULL) {
        if (regexec(&reg_buf, de->d_name, (size_t) 1, pmatch, 0) == 0) {
            if (is_card_ep11_and_online(de->d_name) != CKR_OK)
                continue;

            if (scan_for_card_domains(de->d_name, handler, handler_data) !=
                CKR_OK)
                break;
        }
    }

    closedir(d);
    regfree(&reg_buf);

    return CKR_OK;
}

static CK_RV handle_all_ep11_cards(ep11_target_t *ep11_targets,
                                   adapter_handler_t handler,
                                   void *handler_data)
{
    int i;
    CK_RV rc;

    if (ep11_targets->length > 0) {
        /* APQN_WHITELIST is specified */
        for (i = 0; i < ep11_targets->length; i++) {
            rc = handler(ep11_targets->apqns[2 * i],
                         ep11_targets->apqns[2 * i + 1], handler_data);
            if (rc != CKR_OK)
                return rc;
        }
    } else {
        /* APQN_ANY used, scan sysfs for available cards */
        return scan_for_ep11_cards(handler, handler_data);
    }

    return CKR_OK;
}

static CK_RV logout_session_obj(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE obj,
                                CK_BYTE *pin_blob, CK_ULONG pin_blob_size,
                                CK_BYTE *session_id, CK_ULONG session_id_len,
                                ep11_target_t *ep11_targets,
                                pid_t pid, CK_DATE *date)
{
    CK_RV rc;
    CK_ULONG i;

    UNUSED(pin_blob_size);

    for (i = 0; i < session_id_len; i++)
        printf("%02X", session_id[i]);
    printf(":\n");
    if (is_process_running(pid))
        printf("\tPid:\t%u (still running)\n", pid);
    else
        printf("\tPid:\t%u\n", pid);
    printf("\tDate:\t%.4s/%.2s/%.2s\n", date->year, date->month, date->day);

    if (is_process_running(pid)) {
        printf("\tSession is not logged out, process %u is still running\n",
               pid);
        return CKR_OK;
    }

    error = CKR_OK;
    rc = handle_all_ep11_cards(ep11_targets, logout_handler, pin_blob);
    if (rc != CKR_OK) {
        fprintf(stderr, "handle_all_ep11_cards() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        return rc;
    }
    if (error != CKR_OK) {
        fprintf(stderr,
                "WARNING: Not all APQNs were successfully logged out.\n");
        if (!force) {
            fprintf(stderr,
                    "         Session is not deleted. Specify -force to delete"
                    "it anyway.\n");
            return rc;
        }
    }

    rc = funcs->C_DestroyObject(session, obj);
    if (rc != CKR_OK) {
        fprintf(stderr, "C_DestroyObject() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        return rc;
    }

    if (!force)
        printf("\tSession logged out successfully\n");
    else
        printf("\tSession deleted due to -force option\n");

    count++;

    return CKR_OK;
}



static CK_RV show_session_obj(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE obj,
                              CK_BYTE *pin_blob, CK_ULONG pin_blob_size,
                              CK_BYTE *session_id, CK_ULONG session_id_len,
                              ep11_target_t *ep11_targets,
                              pid_t pid, CK_DATE *date)
{
    CK_ULONG i;

    UNUSED(session);
    UNUSED(obj);
    UNUSED(pin_blob);
    UNUSED(pin_blob_size);
    UNUSED(ep11_targets);

    for (i = 0; i < session_id_len; i++)
        printf("%02X", session_id[i]);
    printf(":\n");
    if (is_process_running(pid))
        printf("\tPid:\t%u (still running)\n", pid);
    else
        printf("\tPid:\t%u\n", pid);
    printf("\tDate:\t%.4s/%.2s/%.2s\n", date->year, date->month, date->day);

    count++;

    return CKR_OK;
}

static CK_BBOOL filter_session(CK_BYTE *session_id, CK_ULONG session_id_len,
                               CK_DATE *date, pid_t pid)
{
    struct tm tm;
    char temp[12];
    char *p;
    time_t t;

    if (filter_sess_id_set) {
        if (session_id_len == sizeof(filter_sess_id) &&
            memcmp(session_id, filter_sess_id, session_id_len) == 0)
            return TRUE;
        return FALSE;
    }

    if (filter_date != -1) {
        memset(&tm, 0, sizeof(tm));
        memcpy(temp, date->year, 4);
        temp[4] = '/';
        memcpy(temp + 5, date->month, 2);
        temp[7] = '/';
        memcpy(temp + 8, date->day, 2);
        temp[10] = '\0';

        p = strptime(temp, "%Y/%m/%d", &tm);
        if (p == NULL || *p != '\0')
            return FALSE;
        t = mktime(&tm);
        if (t == -1)
            return FALSE;
        if (difftime(t, filter_date) <= 0)
            return TRUE;
        return FALSE;
    }

    if (filter_pid != 0) {
        if (pid == filter_pid)
            return TRUE;
        return FALSE;
    }

    return TRUE;
}

static CK_RV process_session_obj(CK_SESSION_HANDLE session,
                                 CK_OBJECT_HANDLE obj, handler_t handler)
{
    CK_RV rc;
    CK_BBOOL match;
    CK_BYTE pin_blob[XCP_PINBLOB_BYTES];
    CK_BYTE session_id[EP11_SESSION_ID_SIZE];
    ep11_target_t ep11_targets;
    pid_t pid;
    CK_DATE date;
    CK_ATTRIBUTE attrs[] = {
        { CKA_VALUE, pin_blob, sizeof(pin_blob) },
        { CKA_ID, session_id, sizeof(session_id) },
        { CKA_APPLICATION, &ep11_targets, sizeof(ep11_targets) },
        { CKA_OWNER, &pid, sizeof(pid) },
        { CKA_START_DATE, &date, sizeof(date) },
    };

    rc = funcs->C_GetAttributeValue(session, obj, attrs,
                                    sizeof(attrs) / sizeof(CK_ATTRIBUTE));
    if (rc != CKR_OK) {
        fprintf(stderr, "C_GetAttributeValue() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));

        /* Invalid CKH_IBM_EP11_SESSION object */
        rc = funcs->C_DestroyObject(session, obj);
        return CKR_OK;
    }

    /* Ignore our own EP11 session */
    if (pid == getpid())
        return CKR_OK;

    match = filter_session(session_id, sizeof(session_id), &date, pid);

    if (match) {
        rc = handler(session, obj, pin_blob, sizeof(pin_blob),
                     session_id, sizeof(session_id), &ep11_targets, pid, &date);
        if (rc != CKR_OK)
            return rc;
    }

    return CKR_OK;
}

static CK_RV find_sessions(CK_SESSION_HANDLE session, handler_t handler)
{
    CK_RV rc;
    CK_OBJECT_HANDLE obj_store[4096];
    CK_ULONG objs_found = 0;
    CK_ULONG obj;
    CK_OBJECT_CLASS class = CKO_HW_FEATURE;
    CK_HW_FEATURE_TYPE type = CKH_IBM_EP11_SESSION;
    CK_BYTE true = TRUE;
    CK_ATTRIBUTE session_template[] = {
        { CKA_CLASS, &class, sizeof(class) },
        { CKA_TOKEN, &true, sizeof(true) },
        { CKA_PRIVATE, &true, sizeof(true) },
        { CKA_HIDDEN, &true, sizeof(true) },
        { CKA_HW_FEATURE_TYPE, &type, sizeof(type) },
    };

    /* find all objects */
    rc = funcs->C_FindObjectsInit(session, session_template,
                                  sizeof(session_template) /
                                  sizeof(CK_ATTRIBUTE));
    if (rc != CKR_OK) {
        fprintf(stderr, "C_FindObjectsInit() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        goto out;
    }

    do {
        rc = funcs->C_FindObjects(session, obj_store, 4096, &objs_found);
        if (rc != CKR_OK) {
            fprintf(stderr, "C_FindObjects() rc = 0x%02lx [%s]\n", rc,
                    p11_get_ckr(rc));
            goto out;
        }

        for (obj = 0; obj < objs_found; obj++) {
            rc = process_session_obj(session, obj_store[obj], handler);
            if (rc != CKR_OK)
                goto out;
        }
    } while (objs_found != 0);

out:
    funcs->C_FindObjectsFinal(session);

    return rc;
}

static CK_RV show_sessions(CK_SESSION_HANDLE session)
{
    CK_RV rc;

    printf("List of EP11 sessions:\n\n");
    count = 0;
    rc = find_sessions(session, show_session_obj);
    if (rc != CKR_OK)
        return rc;
    printf("\n%lu EP11-Sessions displayed\n", count);
    return 0;
}

static CK_RV logout_sessions(CK_SESSION_HANDLE session)
{
    CK_RV rc;

    printf("List of EP11 sessions:\n\n");
    count = 0;
    rc = find_sessions(session, logout_session_obj);
    if (rc != CKR_OK)
        return rc;
    printf("\n%lu EP11-Sessions logged out\n", count);
    return rc;
}

static CK_RV find_vhsmpin_object(CK_SESSION_HANDLE session,
                                 CK_OBJECT_HANDLE *obj)
{
    CK_RV rc;
    CK_OBJECT_HANDLE obj_store[16];
    CK_ULONG objs_found = 0;
    CK_OBJECT_CLASS class = CKO_HW_FEATURE;
    CK_HW_FEATURE_TYPE type = CKH_IBM_EP11_VHSMPIN;
    CK_BYTE true = TRUE;
    CK_ATTRIBUTE vhsmpin_template[] = {
        { CKA_CLASS, &class, sizeof(class) },
        { CKA_TOKEN, &true, sizeof(true) },
        { CKA_PRIVATE, &true, sizeof(true) },
        { CKA_HIDDEN, &true, sizeof(true) },
        { CKA_HW_FEATURE_TYPE, &type, sizeof(type) },
    };

    /* find all objects */
    rc = funcs->C_FindObjectsInit(session, vhsmpin_template,
                                  sizeof(vhsmpin_template) /
                                  sizeof(CK_ATTRIBUTE));
    if (rc != CKR_OK) {
        fprintf(stderr, "C_FindObjectsInit() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        goto out;
    }

    rc = funcs->C_FindObjects(session, obj_store, 16, &objs_found);
    if (rc != CKR_OK) {
        fprintf(stderr, "C_FindObjects() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        goto out;
    }

    if (objs_found > 0)
        *obj = obj_store[0];
    else
        *obj = CK_INVALID_HANDLE;

out:
    funcs->C_FindObjectsFinal(session);

    return rc;
}


static CK_RV set_vhsmpin(CK_SESSION_HANDLE session)
{
    CK_RV rc;
    CK_BYTE vhsm_pin[XCP_MAX_PINBYTES + 1];
    CK_OBJECT_HANDLE obj = CK_INVALID_HANDLE;
    CK_OBJECT_CLASS class = CKO_HW_FEATURE;
    CK_HW_FEATURE_TYPE type = CKH_IBM_EP11_VHSMPIN;
    CK_BYTE subject[] = "EP11 VHSM-Pin Object";
    CK_BYTE true = TRUE;

    if (get_vhsm_pin(vhsm_pin)) {
        fprintf(stderr, "get_vhsm_pin() failed\n");
        return CKR_FUNCTION_FAILED;
    }

    CK_ATTRIBUTE attrs[] = {
        { CKA_CLASS, &class, sizeof(class) },
        { CKA_TOKEN, &true, sizeof(true) },
        { CKA_PRIVATE, &true, sizeof(true) },
        { CKA_HIDDEN, &true, sizeof(true) },
        { CKA_HW_FEATURE_TYPE, &type, sizeof(type) },
        { CKA_SUBJECT, &subject, sizeof(subject) },
        { CKA_VALUE, vhsm_pin, strlen((char *)vhsm_pin) },
    };

    rc = find_vhsmpin_object(session, &obj);
    if (rc != CKR_OK) {
        fprintf(stderr, "find_vhsmpin_object() failed\n");
        return CKR_FUNCTION_FAILED;
    }

    if (obj != CK_INVALID_HANDLE) {
        rc = funcs->C_DestroyObject(session, obj);
        if (rc != CKR_OK) {
            fprintf(stderr, "C_DestroyObject() rc = 0x%02lx [%s]\n", rc,
                    p11_get_ckr(rc));
            return rc;
        }
    }

    rc = funcs->C_CreateObject(session,
                               attrs, sizeof(attrs) / sizeof(CK_ATTRIBUTE),
                               &obj);
    if (rc != CKR_OK) {
        fprintf(stderr, "C_CreateObject() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        return rc;
    }
    printf("VHSM-pin successfully set.\n");

    return CKR_OK;
}

static CK_RV status_handler(uint_32 adapter, uint_32 domain,
                            void *handler_data)
{
    ep11_target_t target_list;
    struct XCP_Module module;
    target_t target = XCP_TGT_INIT;
    uint32_t *res = NULL;
    CK_ULONG reslen = 0;
    CK_ULONG i;
    uint32_t caps = 0;
    CK_RV rc;
    CK_BBOOL found = CK_FALSE;

    UNUSED(handler_data);

    if (dll_m_add_module != NULL) {
        memset(&module, 0, sizeof(module));
        module.version = lib_version.major >= 3 ? XCP_MOD_VERSION_2
                                                : XCP_MOD_VERSION_1;
        module.flags = XCP_MFL_MODULE;
        module.module_nr = adapter;
        XCPTGTMASK_SET_DOM(module.domainmask, domain);
        rc = dll_m_add_module(&module, &target);
        if (rc != 0) {
            fprintf(stderr,
                    "dll_m_add_module (EXT_CAPLIST) failed: rc=0x%lx\n", rc);
            return CKR_FUNCTION_FAILED;
        }
    } else {
        /* Fall back to old target handling */
        memset(&target_list, 0, sizeof(ep11_target_t));
        target_list.length = 1;
        target_list.apqns[0] = adapter;
        target_list.apqns[1] = domain;
        target = (target_t)&target_list;
    }

    printf("APQN %02x.%04x:\n", adapter, domain);

    reslen = sizeof(caps);
    rc = dll_m_get_xcp_info(&caps, &reslen, CK_IBM_XCPQ_EXT_CAPS, 0, target);
    if (rc != CKR_OK || reslen != sizeof(caps)) {
        fprintf(stderr,
                "dll_m_get_xcp_info (EXT_CAPS) failed: rc=0x%lx\n", rc);
        goto done;
    }

    if (caps == 0)
        goto no_info;

    reslen = caps * sizeof(uint32_t) * 2;
    res = calloc(1, reslen);
    if (res == NULL) {
        fprintf(stderr, "Failed to allocate memory\n");
        goto done;
    }

    rc = dll_m_get_xcp_info(res, &reslen, CK_IBM_XCPQ_EXT_CAPLIST, 0, target);
    if (rc != CKR_OK) {
        fprintf(stderr,
                "dll_m_get_xcp_info (EXT_CAPLIST) failed: rc=0x%lx\n", rc);
        goto done;
    }

    for (i = 0; i < reslen / 4; i += 2) {
        if (res[i] == CK_IBM_XCPXQ_MAX_SESSIONS) {
            printf("  Max Sessions:        %u\n", res[i + 1]);
            found = CK_TRUE;
        } else if (res[i] == CK_IBM_XCPXQ_AVAIL_SESSIONS) {
            printf("  Available Sessions:  %u\n", res[i + 1]);
            found = CK_TRUE;
        }
    }

no_info:
    if (!found)
        printf("  Information not available\n");

done:
    if (dll_m_rm_module != NULL)
        dll_m_rm_module(&module, target);
    if (res != NULL)
        free(res);

    return CKR_OK;
}

static CK_RV show_ep11_status(void)
{
    ep11_target_t any_target = { 0, 0, { 0 } };
    CK_RV rc;

    rc = handle_all_ep11_cards(&any_target, status_handler, NULL);
    if (rc != CKR_OK) {
        fprintf(stderr, "handle_all_ep11_cards() rc = 0x%02lx [%s]\n", rc,
                p11_get_ckr(rc));
        return rc;
    }

    return CKR_OK;
}

#ifdef EP11_HSMSIM
#define DLOPEN_FLAGS        RTLD_GLOBAL | RTLD_NOW | RTLD_DEEPBIND
#else
#define DLOPEN_FLAGS        RTLD_GLOBAL | RTLD_NOW
#endif

static void *ep11_load_host_lib(void)
{
    void *lib_ep11;
    char *ep11_lib_name;
    char *errstr;

    ep11_lib_name = secure_getenv(EP11SHAREDLIB_NAME);
    if (ep11_lib_name != NULL) {
        lib_ep11 = dlopen(ep11_lib_name, DLOPEN_FLAGS);

        if (lib_ep11 == NULL) {
            errstr = dlerror();
            fprintf(stderr, "Error loading shared library '%s' [%s]\n",
                    ep11_lib_name, errstr);
            return NULL;
        }
        return lib_ep11;
    }

    ep11_lib_name = EP11SHAREDLIB_V4;
    lib_ep11 = dlopen(ep11_lib_name, DLOPEN_FLAGS);

    if (lib_ep11 == NULL) {
        /* Try version 3 instead */
        ep11_lib_name = EP11SHAREDLIB_V3;
        lib_ep11 = dlopen(ep11_lib_name, DLOPEN_FLAGS);
    }

    if (lib_ep11 == NULL) {
        /* Try version 2 instead */
        ep11_lib_name = EP11SHAREDLIB_V2;
        lib_ep11 = dlopen(ep11_lib_name, DLOPEN_FLAGS);
    }

    if (lib_ep11 == NULL) {
        /* Try version 1 instead */
        ep11_lib_name = EP11SHAREDLIB_V1;
        lib_ep11 = dlopen(ep11_lib_name, DLOPEN_FLAGS);
    }

    if (lib_ep11 == NULL) {
        /* Try unversioned library instead */
        ep11_lib_name = EP11SHAREDLIB;
        lib_ep11 = dlopen(ep11_lib_name, DLOPEN_FLAGS);
    }

    if (lib_ep11 == NULL) {
        errstr = dlerror();
        fprintf(stderr, "Error loading shared library '%s[.4|.3|.2|.1]' [%s]\n",
                EP11SHAREDLIB, errstr);
        return NULL;
    }

    return lib_ep11;
}

int main(int argc, char **argv)
{
    int rc;
    void *lib_ep11;
    CK_C_INITIALIZE_ARGS cinit_args;
    CK_BYTE user_pin[PKCS11_MAX_PIN_LEN + 1];
    CK_FLAGS flags;
    CK_SESSION_HANDLE session;
    CK_ULONG user_pin_len;

    rc = do_ParseArgs(argc, argv);
    if (rc != 1)
        return rc;

    /* dynamically load in the ep11 shared library */
    lib_ep11 = ep11_load_host_lib();
    if (!lib_ep11)
        return CKR_FUNCTION_FAILED;

    *(void **)(&dll_m_init) = dlsym(lib_ep11, "m_init");
    *(void **)(&dll_m_Logout) = dlsym(lib_ep11, "m_Logout");
    *(void **)(&dll_m_get_xcp_info) = dlsym(lib_ep11, "m_get_xcp_info");
    if (dll_m_init == NULL || dll_m_Logout == NULL ||
        dll_m_get_xcp_info == NULL) {
        fprintf(stderr, "ERROR loading shared lib '%s' [%s]\n",
                EP11SHAREDLIB, dlerror());
        return CKR_FUNCTION_FAILED;
    }
    /*
     * The following are only available since EP11 host library version 2.
     * Ignore if they fail to load, the code will fall back to the old target
     * handling in this case.
     */
    *(void **)(&dll_m_add_module) = dlsym(lib_ep11, "m_add_module");
    *(void **)(&dll_m_rm_module) = dlsym(lib_ep11, "m_rm_module");
    if (dll_m_add_module == NULL || dll_m_rm_module == NULL) {
        dll_m_add_module = NULL;
        dll_m_rm_module = NULL;
    }

    rc = dll_m_init();
    if (rc != CKR_OK) {
        fprintf(stderr, "ERROR dll_m_init() Failed, rx = 0x%0x\n", rc);
        return rc;
    }

    rc = get_ep11_library_version(&lib_version);
    if (rc != CKR_OK)
        return rc;

    if (action == ACTION_STATUS) {
        rc = show_ep11_status();
        return rc;
    }

    printf("Using slot #%lu...\n\n", SLOT_ID);

    rc = do_GetFunctionList();
    if (!rc) {
        fprintf(stderr, "ERROR do_GetFunctionList() Failed, rx = 0x%0x\n", rc);
        return rc;
    }

    memset(&cinit_args, 0x0, sizeof(cinit_args));
    cinit_args.flags = CKF_OS_LOCKING_OK;

    funcs->C_Initialize(&cinit_args);
    {
        CK_SESSION_HANDLE hsess = 0;
        rc = funcs->C_GetFunctionStatus(hsess);
        if (rc != CKR_FUNCTION_NOT_PARALLEL)
            return rc;

        rc = funcs->C_CancelFunction(hsess);
        if (rc != CKR_FUNCTION_NOT_PARALLEL)
            return rc;
    }

    if (!is_ep11_token(SLOT_ID)) {
        fprintf(stderr, "ERROR Slot %lu is not an EP11 token\n", SLOT_ID);
        return CKR_FUNCTION_FAILED;
    }

    flags = CKF_SERIAL_SESSION | CKF_RW_SESSION;
    rc = funcs->C_OpenSession(SLOT_ID, flags, NULL, NULL, &session);
    if (rc != CKR_OK) {
        fprintf(stderr, "C_OpenSession() rc = 0x%02x [%s]\n", rc,
                p11_get_ckr(rc));
        session = CK_INVALID_HANDLE;
        return rc;
    }

    if (get_user_pin(user_pin)) {
        fprintf(stderr, "get_user_pin() failed\n");
        rc = funcs->C_CloseAllSessions(SLOT_ID);
        if (rc != CKR_OK)
            fprintf(stderr, "C_CloseAllSessions() rc = 0x%02x [%s]\n", rc,
                    p11_get_ckr(rc));
        return rc;
    }

    user_pin_len = (CK_ULONG) strlen((char *) user_pin);
    rc = funcs->C_Login(session, CKU_USER, user_pin, user_pin_len);
    if (rc != CKR_OK) {
        fprintf(stderr, "C_Login() rc = 0x%02x [%s]\n", rc, p11_get_ckr(rc));
        return rc;
    }

    switch (action) {
    case ACTION_SHOW:
        rc = show_sessions(session);
        break;
    case ACTION_LOGOUT:
        rc = logout_sessions(session);
        break;
    case ACTION_VHSMPIN:
        rc = set_vhsmpin(session);
        break;
    }
    if (rc != CKR_OK)
        return rc;

    rc = funcs->C_Logout(session);
    rc = funcs->C_CloseAllSessions(SLOT_ID);

    return rc;
}
