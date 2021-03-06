/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 */

#include "suricata-common.h"
#include "detect.h"
#include "detect-engine.h"
#include "util-hash.h"

#include "util-reference-config.h"
#include "conf.h"
#include "util-unittest.h"
#include "util-error.h"
#include "util-debug.h"
#include "util-fmemopen.h"

/* Regex to parse each line from reference.config file.  The first substring
 * is for the system name and the second for the url */
/*-----------------------------------------------------------system-------------------url----*/
#define SC_RCONF_REGEX "^\\s*config\\s+reference\\s*:\\s*([a-zA-Z][a-zA-Z0-9-_]*)\\s+(.+)\\s*$"

/* Default path for the reference.conf file */
#define SC_RCONF_DEFAULT_FILE_PATH CONFIG_DIR "/reference.config"

/* Holds a pointer to the default path for the reference.config file */
static const char *file_path = SC_RCONF_DEFAULT_FILE_PATH;
static FILE *fd = NULL;
static pcre *regex = NULL;
static pcre_extra *regex_study = NULL;

/* the hash functions */
uint32_t SCRConfReferenceHashFunc(HashTable *ht, void *data, uint16_t datalen);
char SCRConfReferenceHashCompareFunc(void *data1, uint16_t datalen1,
                                     void *data2, uint16_t datalen2);
void SCRConfReferenceHashFree(void *ch);

/* used to get the reference.config file path */
static char *SCRConfGetConfFilename(void);

/**
 * \brief Inits the context to be used by the Reference Config parsing API.
 *
 *        This function initializes the hash table to be used by the Detection
 *        Engine Context to hold the data from reference.config file,
 *        obtains the file descriptor to parse the reference.config file, and
 *        inits the regex used to parse the lines from reference.config file.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
static int SCRConfInitContextAndLocalResources(DetectEngineCtx *de_ctx)
{
    char *filename = NULL;
    const char *eb = NULL;
    int eo;
    int opts = 0;

    /* init the hash table to be used by the reference config references */
    de_ctx->reference_conf_ht = HashTableInit(128, SCRConfReferenceHashFunc,
                                              SCRConfReferenceHashCompareFunc,
                                              SCRConfReferenceHashFree);
    if (de_ctx->reference_conf_ht == NULL) {
        SCLogError(SC_ERR_HASH_TABLE_INIT, "Error initializing the hash "
                   "table");
        goto error;
    }

    /* if it is not NULL, use the file descriptor.  The hack so that we can
     * avoid using a dummy reference file for testing purposes and
     * instead use an input stream against a buffer containing the
     * reference strings */
    if (fd == NULL) {
        filename = SCRConfGetConfFilename();
        if ((fd = fopen(filename, "r")) == NULL) {
            SCLogError(SC_ERR_FOPEN, "Error opening file: \"%s\": %s", filename,
                       strerror(errno));
            goto error;
        }
    }

    regex = pcre_compile(SC_RCONF_REGEX, opts, &eb, &eo, NULL);
    if (regex == NULL) {
        SCLogDebug("Compile of \"%s\" failed at offset %" PRId32 ": %s",
                   SC_RCONF_REGEX, eo, eb);
        goto error;
    }

    regex_study = pcre_study(regex, 0, &eb);
    if (eb != NULL) {
        SCLogDebug("pcre study failed: %s", eb);
        goto error;
    }

    return 0;

 error:
    if (de_ctx->reference_conf_ht != NULL) {
        HashTableFree(de_ctx->reference_conf_ht);
        de_ctx->reference_conf_ht = NULL;
    }
    if (fd != NULL) {
        fclose(fd);
        fd = NULL;
    }

    if (regex != NULL) {
        pcre_free(regex);
        regex = NULL;
    }
    if (regex_study != NULL) {
        pcre_free(regex_study);
        regex_study = NULL;
    }

    return -1;
}


/**
 * \brief Returns the path for the Reference Config file.  We check if we
 *        can retrieve the path from the yaml conf file.  If it is not present,
 *        return the default path for the reference.config file which is
 *        "./reference.config".
 *
 * \retval log_filename Pointer to a string containing the path for the
 *                      reference.config file.
 */
static char *SCRConfGetConfFilename(void)
{
    char *path = NULL;
    if (ConfGet("reference-config-file", &path) != 1) {
        return (char *)file_path;
    }
    return path;
}

/**
 * \brief Releases local resources used by the Reference Config API.
 */
static void SCRConfDeInitLocalResources(DetectEngineCtx *de_ctx)
{
    if (fd != NULL)
        fclose(fd);
    file_path = SC_RCONF_DEFAULT_FILE_PATH;
    fd = NULL;

    if (regex != NULL) {
        pcre_free(regex);
        regex = NULL;
    }
    if (regex_study != NULL) {
        pcre_free(regex_study);
        regex_study = NULL;
    }

    return;
}

/**
 * \brief Releases de_ctx resources related to Reference Config API.
 */
void SCRConfDeInitContext(DetectEngineCtx *de_ctx)
{
    if (de_ctx->reference_conf_ht != NULL)
        HashTableFree(de_ctx->reference_conf_ht);

    de_ctx->reference_conf_ht = NULL;

    return;
}

/**
 * \brief Converts a string to lowercase.
 *
 * \param str Pointer to the string to be converted.
 */
static char *SCRConfStringToLowercase(const char *str)
{
    char *new_str = NULL;
    char *temp_str = NULL;

    if ((new_str = SCStrdup(str)) == NULL) {
        return NULL;
    }

    temp_str = new_str;
    while (*temp_str != '\0') {
        *temp_str = tolower((unsigned char)*temp_str);
        temp_str++;
    }

    return new_str;
}

/**
 * \brief Parses a line from the reference config file and adds it to Reference
 *        Config hash table DetectEngineCtx->reference_conf_ht.
 *
 * \param rawstr Pointer to the string to be parsed.
 * \param de_ctx Pointer to the Detection Engine Context.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
static int SCRConfAddReference(char *rawstr, DetectEngineCtx *de_ctx)
{
    const char *system = NULL;
    const char *url = NULL;

    SCRConfReference *ref_new = NULL;
    SCRConfReference *ref_lookup = NULL;

#define MAX_SUBSTRINGS 30
    int ret = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(regex, regex_study, rawstr, strlen(rawstr), 0, 0, ov, 30);
    if (ret < 0) {
        SCLogError(SC_ERR_REFERENCE_CONFIG, "Invalid Reference Config in "
                   "reference.config file");
        goto error;
    }

    /* retrieve the reference system */
    ret = pcre_get_substring((char *)rawstr, ov, 30, 1, &system);
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring() failed");
        goto error;
    }

    /* retrieve the reference url */
    ret = pcre_get_substring((char *)rawstr, ov, 30, 2, &url);
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring() failed");
        goto error;
    }

    /* Create a new instance of the parsed Reference string */
    ref_new = SCRConfAllocSCRConfReference(system, url);

    /* Check if the Reference is present in the HashTable.  In case it's present
     * ignore it, as it's a duplicate.  If not present, add it to the table */
    ref_lookup = HashTableLookup(de_ctx->reference_conf_ht, ref_new, 0);
    if (ref_lookup == NULL) {
        if (HashTableAdd(de_ctx->reference_conf_ht, ref_new, 0) < 0) {
            SCLogDebug("HashTable Add failed");
        }
    } else {
        SCLogDebug("Duplicate reference found inside reference.config");
        SCRConfDeAllocSCRConfReference(ref_new);
    }

    /* free the substrings */
    pcre_free_substring(system);
    pcre_free_substring(url);
    return 0;

 error:
    if (system)
        pcre_free_substring(system);
    if (url)
        pcre_free_substring(url);

    return -1;
}

/**
 * \brief Checks if a string is a comment or a blank line.
 *
 *        Comments lines are lines of the following format -
 *        "# This is a comment string" or
 *        "   # This is a comment string".
 *
 * \param line String that has to be checked.
 *
 * \retval 1 On the argument string being a comment or blank line.
 * \retval 0 Otherwise.
 */
static int SCRConfIsLineBlankOrComment(char *line)
{
    while (*line != '\0') {
        /* we have a comment */
        if (*line == '#')
            return 1;

        /* this line is neither a comment line, nor a blank line */
        if (!isspace((unsigned char)*line))
            return 0;

        line++;
    }

    /* we have a blank line */
    return 1;
}

/**
 * \brief Parses the Reference Config file and updates the
 *        DetectionEngineCtx->reference_conf_ht with the Reference information.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 */
static void SCRConfParseFile(DetectEngineCtx *de_ctx)
{
    char line[1024];
    uint8_t i = 1;

    while (fgets(line, sizeof(line), fd) != NULL) {
        if (SCRConfIsLineBlankOrComment(line))
            continue;

        SCRConfAddReference(line, de_ctx);
        i++;
    }

#ifdef UNITTESTS
    SCLogInfo("Added \"%d\" reference types from the reference.config file",
              de_ctx->reference_conf_ht->count);
#endif /* UNITTESTS */
    return;
}

/**
 * \brief Returns a new SCRConfReference instance.  The reference string
 *        is converted into lowercase, before being assigned to the instance.
 *
 * \param system  Pointer to the system.
 * \param url     Pointer to the reference url.
 *
 * \retval ref Pointer to the new instance of SCRConfReference.
 */
SCRConfReference *SCRConfAllocSCRConfReference(const char *system,
                                               const char *url)
{
    SCRConfReference *ref = NULL;

    if (system == NULL) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid arguments.  system NULL");
        return NULL;
    }

    if ((ref = SCMalloc(sizeof(SCRConfReference))) == NULL) {
        return NULL;
    }
    memset(ref, 0, sizeof(SCRConfReference));

    if ((ref->system = SCRConfStringToLowercase(system)) == NULL) {
        SCFree(ref);
        return NULL;
    }

    if (url != NULL && (ref->url = SCStrdup(url)) == NULL) {
        SCFree(ref->system);
        SCFree(ref);
        return NULL;
    }

    return ref;
}

/**
 * \brief Frees a SCRConfReference instance.
 *
 * \param Pointer to the SCRConfReference instance that has to be freed.
 */
void SCRConfDeAllocSCRConfReference(SCRConfReference *ref)
{
    if (ref != NULL) {
        if (ref->system != NULL)
            SCFree(ref->system);

        if (ref->url != NULL)
            SCFree(ref->url);

        SCFree(ref);
    }

    return;
}

/**
 * \brief Hashing function to be used to hash the Reference name.  Would be
 *        supplied as an argument to the HashTableInit function for
 *        DetectEngineCtx->reference_conf_ht.
 *
 * \param ht      Pointer to the HashTable.
 * \param data    Pointer to the data to be hashed.  In this case, the data
 *                would be a pointer to a SCRConfReference instance.
 * \param datalen Not used by this function.
 */
uint32_t SCRConfReferenceHashFunc(HashTable *ht, void *data, uint16_t datalen)
{
    SCRConfReference *ref = (SCRConfReference *)data;
    uint32_t hash = 0;
    int i = 0;

    int len = strlen(ref->system);

    for (i = 0; i < len; i++)
        hash += tolower((unsigned char)ref->system[i]);

    hash = hash % ht->array_size;

    return hash;
}

/**
 * \brief Used to compare two References that have been stored in the HashTable.
 *        This function is supplied as an argument to the HashTableInit function
 *        for DetectionEngineCtx->reference_conf_ct.
 *
 * \param data1 Pointer to the first SCRConfReference to be compared.
 * \param len1  Not used by this function.
 * \param data2 Pointer to the second SCRConfReference to be compared.
 * \param len2  Not used by this function.
 *
 * \retval 1 On data1 and data2 being equal.
 * \retval 0 On data1 and data2 not being equal.
 */
char SCRConfReferenceHashCompareFunc(void *data1, uint16_t datalen1,
                                     void *data2, uint16_t datalen2)
{
    SCRConfReference *ref1 = (SCRConfReference *)data1;
    SCRConfReference *ref2 = (SCRConfReference *)data2;
    int len1 = 0;
    int len2 = 0;

    if (ref1 == NULL || ref2 == NULL)
        return 0;

    if (ref1->system == NULL || ref2->system == NULL)
        return 0;

    len1 = strlen(ref1->system);
    len2 = strlen(ref2->system);

    if (len1 == len2 && memcmp(ref1->system, ref2->system, len1) == 0) {
        SCLogDebug("Match found inside Reference-Config hash function");
        return 1;
    }

    return 0;
}

/**
 * \brief Used to free the Reference Config Hash Data that was stored in
 *        DetectEngineCtx->reference_conf_ht Hashtable.
 *
 * \param data Pointer to the data that has to be freed.
 */
void SCRConfReferenceHashFree(void *data)
{
    SCRConfDeAllocSCRConfReference(data);

    return;
}

/**
 * \brief Loads the Reference info from the reference.config file.
 *
 *        The reference.config file contains references that can be used in
 *        Signatures.  Each line of the file should  have the following format -
 *        config reference: system_name, reference_url.
 *
 * \param de_ctx Pointer to the Detection Engine Context that should be updated
 *               with reference information.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int SCRConfLoadReferenceConfigFile(DetectEngineCtx *de_ctx)
{
    if (SCRConfInitContextAndLocalResources(de_ctx) == -1) {
        SCLogInfo("Please check the \"reference-config-file\" option in your suricata.yaml file");
        exit(EXIT_FAILURE);
    }

    SCRConfParseFile(de_ctx);
    SCRConfDeInitLocalResources(de_ctx);

    return 0;
}

/**
 * \brief Gets the refernce config from the corresponding hash table stored
 *        in the Detection Engine Context's reference conf ht, given the
 *        reference name.
 *
 * \param ct_name Pointer to the reference name that has to be looked up.
 * \param de_ctx  Pointer to the Detection Engine Context.
 *
 * \retval lookup_rconf_info Pointer to the SCRConfReference instance from
 *                           the hash table on success; NULL on failure.
 */
SCRConfReference *SCRConfGetReference(const char *rconf_name,
                                      DetectEngineCtx *de_ctx)
{
    SCRConfReference *ref_conf = SCRConfAllocSCRConfReference(rconf_name, NULL);
    if (ref_conf == NULL)
        exit(EXIT_FAILURE);
    SCRConfReference *lookup_ref_conf = HashTableLookup(de_ctx->reference_conf_ht,
                                                        ref_conf, 0);

    SCRConfDeAllocSCRConfReference(ref_conf);
    return lookup_ref_conf;
}

/*----------------------------------Unittests---------------------------------*/


#ifdef UNITTESTS

/**
 * \brief Creates a dummy reference config, with all valid references, for
 *        testing purposes.
 */
void SCRConfGenerateValidDummyReferenceConfigFD01(void)
{
    const char *buffer =
        "config reference: one http://www.one.com\n"
        "config reference: two http://www.two.com\n"
        "config reference: three http://www.three.com\n"
        "config reference: one http://www.one.com\n"
        "config reference: three http://www.three.com\n";

    fd = SCFmemopen((void *)buffer, strlen(buffer), "r");
    if (fd == NULL)
        SCLogDebug("Error with SCFmemopen() called by Reference Config test code");

    return;
}

/**
 * \brief Creates a dummy reference config, with some valid references and a
 *        couple of invalid references, for testing purposes.
 */
void SCRConfGenerateInValidDummyReferenceConfigFD02(void)
{
    const char *buffer =
        "config reference: one http://www.one.com\n"
        "config_ reference: two http://www.two.com\n"
        "config reference_: three http://www.three.com\n"
        "config reference: four\n"
        "config reference five http://www.five.com\n";

    fd = SCFmemopen((void *)buffer, strlen(buffer), "r");
    if (fd == NULL)
        SCLogDebug("Error with SCFmemopen() called by Reference Config test code");

    return;
}

/**
 * \brief Creates a dummy reference config, with all invalid references, for
 *        testing purposes.
 */
void SCRConfGenerateInValidDummyReferenceConfigFD03(void)
{
    const char *buffer =
        "config reference one http://www.one.com\n"
        "config_ reference: two http://www.two.com\n"
        "config reference_: three http://www.three.com\n"
        "config reference: four\n";

    fd = SCFmemopen((void *)buffer, strlen(buffer), "r");
    if (fd == NULL)
        SCLogDebug("Error with SCFmemopen() called by Reference Config test code");

    return;
}

/**
 * \brief Deletes the FD, if set by the other testing functions.
 */
void SCRConfDeleteDummyReferenceConfigFD(void)
{
    if (fd != NULL) {
        fclose(fd);
        fd = NULL;
    }

    return;
}

/**
 * \test Check that the reference file is loaded and the detection engine
 *       content reference_conf_ht loaded with the reference data.
 */
int SCRConfTest01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 0;

    if (de_ctx == NULL)
        return result;

    SCRConfGenerateValidDummyReferenceConfigFD01();
    SCRConfLoadReferenceConfigFile(de_ctx);
    SCRConfDeleteDummyReferenceConfigFD();

    if (de_ctx->reference_conf_ht == NULL)
        goto end;

    result = (de_ctx->reference_conf_ht->count == 3);
    if (result == 0)
        printf("FAILED: de_ctx->reference_conf_ht->count %u: ", de_ctx->reference_conf_ht->count);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Check that invalid references present in the reference.config file
 *       aren't loaded.
 */
int SCRConfTest02(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 0;

    if (de_ctx == NULL)
        return result;

    SCRConfGenerateInValidDummyReferenceConfigFD03();
    SCRConfLoadReferenceConfigFile(de_ctx);
    SCRConfDeleteDummyReferenceConfigFD();

    if (de_ctx->reference_conf_ht == NULL)
        goto end;

    result = (de_ctx->reference_conf_ht->count == 0);


 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Check that only valid references are loaded into the hash table from
 *       the reference.config file.
 */
int SCRConfTest03(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 0;

    if (de_ctx == NULL)
        return result;

    SCRConfGenerateInValidDummyReferenceConfigFD02();
    SCRConfLoadReferenceConfigFile(de_ctx);
    SCRConfDeleteDummyReferenceConfigFD();

    if (de_ctx->reference_conf_ht == NULL)
        goto end;

    result = (de_ctx->reference_conf_ht->count == 1);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Check if the reference info from the reference.config file have
 *       been loaded into the hash table.
 */
int SCRConfTest04(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 1;

    if (de_ctx == NULL)
        return 0;

    SCRConfGenerateValidDummyReferenceConfigFD01();
    SCRConfLoadReferenceConfigFile(de_ctx);
    SCRConfDeleteDummyReferenceConfigFD();

    if (de_ctx->reference_conf_ht == NULL)
        goto end;

    result = (de_ctx->reference_conf_ht->count == 3);

    result &= (SCRConfGetReference("one", de_ctx) != NULL);
    result &= (SCRConfGetReference("two", de_ctx) != NULL);
    result &= (SCRConfGetReference("three", de_ctx) != NULL);
    result &= (SCRConfGetReference("four", de_ctx) == NULL);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Check if the reference info from the invalid reference.config file
 *       have not been loaded into the hash table, and cross verify to check
 *       that the hash table contains no reference data.
 */
int SCRConfTest05(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 1;

    if (de_ctx == NULL)
        return 0;

    SCRConfGenerateInValidDummyReferenceConfigFD03();
    SCRConfLoadReferenceConfigFile(de_ctx);
    SCRConfDeleteDummyReferenceConfigFD();

    if (de_ctx->reference_conf_ht == NULL)
        goto end;

    result = (de_ctx->reference_conf_ht->count == 0);

    result &= (SCRConfGetReference("one", de_ctx) == NULL);
    result &= (SCRConfGetReference("two", de_ctx) == NULL);
    result &= (SCRConfGetReference("three", de_ctx) == NULL);
    result &= (SCRConfGetReference("four", de_ctx) == NULL);
    result &= (SCRConfGetReference("five", de_ctx) == NULL);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Check if the reference info from the reference.config file have
 *       been loaded into the hash table.
 */
int SCRConfTest06(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 1;

    if (de_ctx == NULL)
        return 0;

    SCRConfGenerateInValidDummyReferenceConfigFD02();
    SCRConfLoadReferenceConfigFile(de_ctx);
    SCRConfDeleteDummyReferenceConfigFD();

    if (de_ctx->reference_conf_ht == NULL)
        goto end;

    result = (de_ctx->reference_conf_ht->count == 1);

    result &= (SCRConfGetReference("one", de_ctx) != NULL);
    result &= (SCRConfGetReference("two", de_ctx) == NULL);
    result &= (SCRConfGetReference("three", de_ctx) == NULL);
    result &= (SCRConfGetReference("four", de_ctx) == NULL);
    result &= (SCRConfGetReference("five", de_ctx) == NULL);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

#endif /* UNITTESTS */

/**
 * \brief This function registers unit tests for Reference Config API.
 */
void SCRConfRegisterTests(void)
{

#ifdef UNITTESTS
    UtRegisterTest("SCRConfTest01", SCRConfTest01, 1);
    UtRegisterTest("SCRConfTest02", SCRConfTest02, 1);
    UtRegisterTest("SCRConfTest03", SCRConfTest03, 1);
    UtRegisterTest("SCRConfTest04", SCRConfTest04, 1);
    UtRegisterTest("SCRConfTest05", SCRConfTest05, 1);
    UtRegisterTest("SCRConfTest06", SCRConfTest06, 1);
#endif /* UNITTESTS */

    return;
}
