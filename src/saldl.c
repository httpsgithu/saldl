/*
    This file is a part of saldl.

    Copyright (C) 2014-2015 Mohammad AlSaleh <CE.Mohammad.AlSaleh at gmail.com>
    https://saldl.github.io

    saldl is free software: you can redistribute it and/or modify
    it under the terms of the Affero GNU General Public License as
    published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Affero GNU General Public License for more details.

    You should have received a copy of the Affero GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "saldl.h"
#include "resume.h"
#include "queue.h"
#include "exit.h"

info_s *info_global = NULL; /* Referenced in the signal handler */

static void saldl_free_all(info_s *info_ptr) {
  saldl_params *params_ptr = info_ptr->params;

  /* Make valgrind happy */
  saldl_free(info_ptr->chunks);
  saldl_free(info_ptr->threads);
  saldl_free(info_ptr->content_type);
  saldl_free(params_ptr->url);
  saldl_free(params_ptr->filename);
  saldl_free(params_ptr->attachment_filename);
  saldl_free(params_ptr->inline_cookies);
  saldl_free(params_ptr->root_dir);
  saldl_free(params_ptr->user_agent);
  saldl_free(params_ptr->post);
  saldl_free(params_ptr->raw_post);
  saldl_free(params_ptr->cookie_file);
  saldl_free(params_ptr->referer);
  saldl_free(params_ptr->proxy);
  saldl_free(params_ptr->tunnel_proxy);
}

int saldl(saldl_params *params_ptr) {
  /* Definitions */
  info_s info = {0};
  info.params = params_ptr;

  /* Activate pthreads support in libevent */
  evthread_use_pthreads();

  /* Handle signals */
  info_global = &info;
  saldl_handle_signals();

  /* Need to be set as early as possible */
  set_color(&params_ptr->no_color);
  set_verbosity(&params_ptr->verbosity, &params_ptr->libcurl_verbosity);

  /* should run only once */
  curl_global_init(CURL_GLOBAL_ALL);

  /* get/set initial info */
  check_url(params_ptr->url);
  remote_info(&info);
  set_info(&info);
  check_remote_file_size(&info);

  /* initialize chunks early for extra_resume() */
  chunks_init(&info);

  if (params_ptr->resume) {
    check_resume(&info);
  }

  print_chunk_info(&info);
  global_progress_init(&info);

  /* exit here if dry_run was set */
  if ( params_ptr->dry_run  ) {
    info_msg(NULL, "Dry-run done.\n");
    saldl_free_all(&info);
    return 0;
  }

  if (params_ptr->resume) {
    if ( !(info.file = fopen(info.part_filename,"rb+")) ) {
      fatal(NULL, "Failed to open %s for writing: %s\n", info.part_filename, strerror(errno));
    }
  }
  else {
    if ( !params_ptr->force  && !access(info.part_filename,F_OK)) {
      fatal(NULL, "%s exists, enable 'resume' or 'force' to overwrite.\n", info.part_filename);
    }
    if ( !(info.file = fopen(info.part_filename,"wb")) ) {
      fatal(NULL, "Failed to open %s for writing: %s\n", info.part_filename, strerror(errno));
    }
  }
  set_modes(&info); /* Keep it between opening file and ctrl_file */

  info.ctrl_file = fopen(info.ctrl_filename,"wb+");
  if (!info.ctrl_file) {
    fatal(FN, "Failed to open '%s' for read/write : %s\n", info.ctrl_filename, strerror(errno) );
  }

  /* Check if download was interrupted after all data was merged */
  if (info.already_finished) {
    goto saldl_all_data_merged;
  }

  /* threads */
  info.threads = saldl_calloc(params_ptr->num_connections, sizeof(thread_s));
  set_reset_storage(&info);

  /* 1st iteration */
  for (size_t counter = 0; counter < params_ptr->num_connections; counter++) {
    queue_next_chunk(&info, counter, 1);
  }

  /* Create event pthreads */
  saldl_pthread_create(&info.trigger_events_pth, NULL, events_trigger_thread, &info);
  saldl_pthread_create(&info.sync_ctrl_pth, NULL, sync_ctrl, &info);
  if (info.chunk_count != 1) {
    saldl_pthread_create(&info.status_display_pth, NULL, status_display, &info);
    saldl_pthread_create(&info.queue_next_pth, NULL, queue_next_thread, &info);
    saldl_pthread_create(&info.merger_pth, NULL, merger_thread, &info);
  }

  /* Now that everything is initialized */
  info.session_status = SESSION_IN_PROGRESS;

  /* Avoid race in joining event threads if the session was interrupted, or finishing without downloading if single_mode */
  do {
    usleep(100000);
  } while (params_ptr->single_mode ? info.chunks[0].progress != PRG_FINISHED : info.global_progress.complete_size != info.file_size);

  /* Join event pthreads */
  join_event_pth(&info.ev_ctrl ,&info.sync_ctrl_pth);

  if (info.chunk_count !=1) {
    join_event_pth(&info.ev_status, &info.status_display_pth);
    join_event_pth(&info.ev_queue, &info.queue_next_pth);
    join_event_pth(&info.ev_merge, &info.merger_pth);
  }

  info.events_queue_done = true;
  event_queue(&info.ev_trigger, NULL);
  join_event_pth(&info.ev_trigger ,&info.trigger_events_pth);

saldl_all_data_merged:

  /* Remove tmp_dirname */
  if ( !params_ptr->mem_bufs && !params_ptr->single_mode ) {
    if ( rmdir(info.tmp_dirname) ) {
      err_msg(NULL, "Failed to delete %s: %s\n", info.tmp_dirname, strerror(errno) );
    }
  }

  /*** Final Steps ***/

  /* One last check  */
  if (info.file_size && (!info.content_encoded || params_ptr->no_decompress) && !params_ptr->no_remote_info) {
    off_t saved_file_size = saldl_fsizeo(info.part_filename, info.file);
    if (saved_file_size != info.file_size) {
      pre_fatal(FN, "Unexpected saved file size (%ju!=%ju).\n", saved_file_size, info.file_size);
      pre_fatal(FN, "This could happen if you're downloading from a dynamic site.\n");
      pre_fatal(FN, "If that's the case and the download is small, retry with --no-remote-info\n");
      fatal(FN, "If you think that's a bug in saldl, report it: https://github.com/saldl/saldl/issues\n");
    }
  }
  else {
    debug_msg(FN, "Strict check for finished file size skipped.\n");
  }

  saldl_fclose(info.part_filename, info.file);
  if (rename(info.part_filename, params_ptr->filename) ) {
    err_msg(NULL, "Failed to rename now-complete %s to %s: %s\n", info.part_filename, params_ptr->filename, strerror(errno));
  }

  saldl_fclose(info.ctrl_filename, info.ctrl_file);
  if ( remove(info.ctrl_filename) ) {
    err_msg(NULL, "Failed to remove %s: %s\n", info.ctrl_filename, strerror(errno));
  }

  /* cleanups */
  curl_cleanup(&info);
  saldl_free_all(&info);

  fprintf(stderr, "%s\n", finish_msg);
  return 0;
}

/* vim: set filetype=c ts=2 sw=2 et spell foldmethod=syntax: */
