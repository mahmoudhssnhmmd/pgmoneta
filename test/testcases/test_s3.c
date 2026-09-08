/*
 * Copyright (C) 2026 The pgmoneta community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Storage-engine integration tests: S3 via Garage.
 *
 * The entire backend lifecycle (start the Garage container, provision it,
 * configure and start a dedicated pgmoneta, tear everything down) is handled
 * by mctf_se. A test only states intent.
 */

#include <mctf.h>
#include <mctf_container.h>
#include <mctf_se.h>
#include <tscommon.h>
#include <utils.h>

#include <stdlib.h>
#include <string.h>
#include <manifest.h>

static int storage_status = MCTF_FAIL;
static char shared_label[256];

/* Return the lexicographically largest (newest) backup label for primary. */
static int
newest_backup_label(char* out, size_t size)
{
   char backup_dir[MAX_PATH];
   char** dirs = NULL;
   int ndir = 0;
   int best = -1;

   snprintf(backup_dir, sizeof(backup_dir), "%s/backup/primary/backup", mctf_se_run_dir());
   pgmoneta_get_directories(backup_dir, &ndir, &dirs);
   if (ndir <= 0 || dirs == NULL)
      return MCTF_FAIL;

   for (int i = 0; i < ndir; i++)
   {
      if (best < 0 || strcmp(dirs[i], dirs[best]) > 0)
         best = i;
   }
   snprintf(out, size, "%s", dirs[best]);

   for (int i = 0; i < ndir; i++)
      free(dirs[i]);
   free(dirs);

   return out[0] != '\0' ? MCTF_OK : MCTF_FAIL;
}

MCTF_MODULE_SETUP(s3)
{
   memset(shared_label, 0, sizeof(shared_label));
   storage_status = mctf_se_up(MCTF_BACKEND_GARAGE);
   if (storage_status == MCTF_OK)
   {
      if (mctf_se_backup("primary") != 0 ||
          newest_backup_label(shared_label, sizeof(shared_label)) != MCTF_OK)
      {
         storage_status = MCTF_FAIL;
      }
   }
}

MCTF_MODULE_TEARDOWN(s3)
{
   mctf_se_down();
}

/*
 * Backup to S3 succeeds and the local catalog records it. Local metadata
 * must always be present and authoritative, even with a remote-only engine.
 */
MCTF_INTEGRATION_TEST(test_s3_backup_keeps_local_metadata)
{
   char* listing = NULL;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_has_local_metadata("primary"), cleanup,
               "local metadata missing after S3 backup");

   MCTF_ASSERT(mctf_se_list_backup("primary", &listing) == 0, cleanup, "list-backup failed");
   MCTF_ASSERT_PTR_NONNULL(listing, cleanup, "empty list-backup response");

cleanup:
   free(listing);
   MCTF_FINISH();
}

/*
 * Full round trip: back up to S3, then restore FROM S3 and verify the data
 * directory was genuinely reconstructed (not just an exit-0 with no files).
 */
MCTF_INTEGRATION_TEST(test_s3_backup_restore_roundtrip)
{
   char* out = NULL;
   char cmd[2 * MAX_PATH];

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   /* download from S3 */
   snprintf(cmd, sizeof(cmd), "s3 restore primary %s %s", shared_label, TEST_RESTORE_DIR);
   MCTF_ASSERT(mctf_se_cli(cmd, NULL) == 0, cleanup, "s3 restore failed");

   /* verify the cluster actually came back (guards against a false-positive
    * restore that returns 0 but writes nothing) */
   mctf_sh(&out, "find %s -name PG_VERSION | head -1 | tr -d '\\n'", TEST_RESTORE_DIR);
   MCTF_ASSERT_PTR_NONNULL(out, cleanup, "restore produced no output");
   MCTF_ASSERT(out[0] != '\0', cleanup, "restore produced no PG_VERSION (empty restore)");

cleanup:
   free(out);
   MCTF_FINISH();
}

/*
 * After a backup, the three mandatory metadata files (backup.info,
 * backup.sha512, backup.manifest) must appear as objects in S3.
 */
MCTF_INTEGRATION_TEST(test_s3_list_has_metadata_files)
{
   char* listing = NULL;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_s3_ls("primary", shared_label, &listing) == 0, cleanup, "s3 ls failed");
   MCTF_ASSERT_PTR_NONNULL(listing, cleanup, "empty s3 ls response");

   MCTF_ASSERT(strstr(listing, "backup.info") != NULL, cleanup,
               "backup.info not found in S3 objects");
   MCTF_ASSERT(strstr(listing, "backup.sha512") != NULL, cleanup,
               "backup.sha512 not found in S3 objects");
   MCTF_ASSERT(strstr(listing, "backup.manifest") != NULL, cleanup,
               "backup.manifest not found in S3 objects");

cleanup:
   free(listing);
   MCTF_FINISH();
}

/*
 * After delete, no S3 objects must remain for that backup label — the
 * remote store must be swept, not just the local catalog entry.
 */
MCTF_INTEGRATION_TEST(test_s3_delete_removes_objects)
{
   char label[256] = {0};
   char* listing = NULL;
   int ls_rc;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_backup("primary") == 0, cleanup, "backup to S3 failed");
   MCTF_ASSERT(newest_backup_label(label, sizeof(label)) == MCTF_OK, cleanup,
               "could not resolve backup label");

   MCTF_ASSERT(mctf_se_delete("primary", label) == 0, cleanup, "delete command failed");

   /* Either the label is gone from the catalog (ls fails) or the object list
    * is empty — both prove the remote store was swept. */
   ls_rc = mctf_se_s3_ls("primary", label, &listing);
   MCTF_ASSERT(ls_rc != 0 || listing == NULL || strstr(listing, "S3Key") == NULL,
               cleanup, "S3 objects remain after delete");

cleanup:
   free(listing);
   MCTF_FINISH();
}

MCTF_INTEGRATION_TEST(test_s3_list_nonexistent_backup_is_empty)
{
   char* listing = NULL;
   char invalid_label[256];

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }

   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   snprintf(invalid_label, sizeof(invalid_label), "%s-invalid", shared_label);

   MCTF_ASSERT(mctf_se_s3_ls("primary", invalid_label, &listing) == 0,
               cleanup,
               "s3 ls failed for non-existent backup");

   MCTF_ASSERT_PTR_NONNULL(listing, cleanup, "empty s3 ls response");

   MCTF_ASSERT(strstr(listing, "S3Key") == NULL,
               cleanup,
               "non-existent backup returned S3 objects");

cleanup:
   free(listing);
   MCTF_FINISH();
}

MCTF_INTEGRATION_TEST(test_s3_backups_are_isolated)
{
   char first_label[256] = {0};
   char second_label[256] = {0};
   char* first_listing = NULL;
   char* second_listing = NULL;
   char first_prefix[512];
   char second_prefix[512];

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }

   MCTF_ASSERT(storage_status == MCTF_OK, cleanup,
               "storage backend setup failed");

   MCTF_ASSERT(mctf_se_backup("primary") == 0,
               cleanup,
               "first backup failed");

   MCTF_ASSERT(newest_backup_label(first_label, sizeof(first_label)) == MCTF_OK,
               cleanup,
               "could not resolve first backup label");

   MCTF_ASSERT(mctf_se_backup("primary") == 0,
               cleanup,
               "second backup failed");

   MCTF_ASSERT(newest_backup_label(second_label, sizeof(second_label)) == MCTF_OK,
               cleanup,
               "could not resolve second backup label");

   MCTF_ASSERT(strcmp(first_label, second_label) != 0,
               cleanup,
               "two backups have the same label");

   MCTF_ASSERT(mctf_se_s3_ls("primary", first_label, &first_listing) == 0,
               cleanup,
               "listing first backup failed");

   MCTF_ASSERT(mctf_se_s3_ls("primary", second_label, &second_listing) == 0,
               cleanup,
               "listing second backup failed");

   snprintf(first_prefix, sizeof(first_prefix),
            "\"S3Key\": \"primary/backup/%s/",
            first_label);

   snprintf(second_prefix, sizeof(second_prefix),
            "\"S3Key\": \"primary/backup/%s/",
            second_label);

   MCTF_ASSERT(strstr(first_listing, first_prefix) != NULL,
               cleanup,
               "first backup objects not found");

   MCTF_ASSERT(strstr(second_listing, second_prefix) != NULL,
               cleanup,
               "second backup objects not found");

   MCTF_ASSERT(strstr(first_listing, second_label) == NULL,
               cleanup,
               "first backup listing contains second backup objects");

   MCTF_ASSERT(strstr(second_listing, first_label) == NULL,
               cleanup,
               "second backup listing contains first backup objects");

cleanup:
   free(first_listing);
   free(second_listing);
   MCTF_FINISH();
}

MCTF_INTEGRATION_TEST(test_s3_backup_content_validation)
{
   char label[256] = {0};
   char manifest_path[MAX_PATH];
   char restored_path[MAX_PATH];
   char command[2 * MAX_PATH];
   char* restored_file = NULL;
   struct deque* paths = NULL;

   const char* files[] =
      {
         "PG_VERSION",
         "postgresql.auto.conf",
         "base/1/PG_VERSION"};

   const int file_count = sizeof(files) / sizeof(files[0]);

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }

   MCTF_ASSERT(storage_status == MCTF_OK, cleanup,
               "storage backend setup failed");

   MCTF_ASSERT(mctf_se_backup("primary") == 0,
               cleanup,
               "S3 backup failed");

   MCTF_ASSERT(newest_backup_label(label, sizeof(label)) == MCTF_OK,
               cleanup,
               "could not resolve backup label");

   snprintf(manifest_path, sizeof(manifest_path),
            "%s/backup/primary/backup/%s/backup.manifest",
            mctf_se_run_dir(), label);

   MCTF_ASSERT(pgmoneta_manifest_get_paths(manifest_path, &paths) == 0,
               cleanup,
               "could not read backup.manifest");

   MCTF_ASSERT_PTR_NONNULL(paths, cleanup,
                           "backup.manifest produced no entries");

   MCTF_ASSERT(mctf_se_restore("primary", label, TEST_RESTORE_DIR) == 0,
               cleanup,
               "S3 restore failed");

   for (int i = 0; i < file_count; i++)
   {
      char* expected_checksum = NULL;
      char* checksum = NULL;

      MCTF_ASSERT(pgmoneta_deque_exists(paths, (char*)files[i]),
                  cleanup,
                  "file is missing from backup.manifest");

      expected_checksum =
         (char*)pgmoneta_deque_get(paths, (char*)files[i]);

      MCTF_ASSERT_PTR_NONNULL(expected_checksum, cleanup,
                              "file checksum is missing from manifest");

      memset(restored_path, 0, sizeof(restored_path));

      snprintf(command, sizeof(command),
               "find '%s' -type f -path '*/%s' -print -quit | tr -d '\\n'",
               TEST_RESTORE_DIR,
               files[i]);

      MCTF_ASSERT(mctf_sh(&restored_file, "%s", command) == 0,
                  cleanup,
                  "could not locate restored file");

      MCTF_ASSERT_PTR_NONNULL(restored_file, cleanup,
                              "restored file path is missing");

      MCTF_ASSERT(restored_file[0] != '\0', cleanup,
                  "restored file does not exist");

      MCTF_ASSERT(mctf_sh(&checksum,
                          "sha512sum '%s' | awk '{print $1}' | tr -d '\\n'",
                          restored_file) == 0,
                  cleanup,
                  "could not calculate restored file checksum");

      MCTF_ASSERT_PTR_NONNULL(checksum, cleanup,
                              "restored file checksum is missing");

      MCTF_ASSERT(checksum[0] != '\0', cleanup,
                  "restored file checksum is empty");

      MCTF_ASSERT(strcmp(checksum, expected_checksum) == 0,
                  cleanup,
                  "restored file checksum does not match backup.manifest");

      free(checksum);
      checksum = NULL;

      free(restored_file);
      restored_file = NULL;
   }

cleanup:
   free(restored_file);

   if (paths != NULL)
   {
      pgmoneta_deque_destroy(paths);
   }

   MCTF_FINISH();
}

MCTF_INTEGRATION_TEST(test_s3_restore_invalid_label_fails)
{
   char invalid_label[256];
   char cmd[2 * MAX_PATH];
   char* output = NULL;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   snprintf(invalid_label, sizeof(invalid_label), "%s-invalid", shared_label);
   snprintf(cmd, sizeof(cmd), "s3 restore primary %s %s", invalid_label, TEST_RESTORE_DIR);

   mctf_se_cli(cmd, &output);

   MCTF_ASSERT_PTR_NONNULL(output, cleanup, "no output from s3 restore command");
   MCTF_ASSERT(strstr(output, "Status: false") != NULL, cleanup,
               "s3 restore of a non-existent backup label did not report failure status");

cleanup:
   free(output);
   MCTF_FINISH();
}

MCTF_INTEGRATION_TEST(test_s3_delete_invalid_label_fails)
{
   char invalid_label[256];
   char cmd[512];
   char* output = NULL;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   snprintf(invalid_label, sizeof(invalid_label), "%s-invalid", shared_label);
   snprintf(cmd, sizeof(cmd), "delete primary %s", invalid_label);

   mctf_se_cli(cmd, &output);

   MCTF_ASSERT_PTR_NONNULL(output, cleanup, "no output from delete command");
   MCTF_ASSERT(strstr(output, "Status: false") != NULL, cleanup,
               "delete of a non-existent backup label did not report failure status");

cleanup:
   free(output);
   MCTF_FINISH();
}

