<template>
  <div class="troubleshoot-root">
    <h1 class="text-2xl font-semibold tracking-tight text-dark dark:text-light">
      {{ $t('troubleshooting.troubleshooting') }}
    </h1>

    <!-- Persistent banner shown while the GPU/WDDM stack is mid-recovery
         from a TDR. Driven by /api/health/tdr.recovery_recent. -->
    <n-alert
      v-if="tdrRecoveryRecent"
      type="warning"
      :show-icon="true"
      class="mb-3 rounded-xl"
      :title="
        translate(
          'troubleshooting.vdd_failure_banner_title',
          'Virtual Display Adapter Failure detected',
        )
      "
    >
      <p class="text-sm">
        {{
          translate(
            'troubleshooting.vdd_failure_banner_body',
            'LuminalShine detected a GPU TDR (Timeout Detection and Recovery) event. New streaming sessions are temporarily refused while the display stack recovers. If a session was active it has been ended; the Moonlight client should reconnect once the recovery completes.',
          )
        }}
      </p>
      <div v-if="tdrLast" class="mt-2 text-xs opacity-80">
        {{ translate('troubleshooting.tdr_last_source', 'Source') }}:
        <span class="font-mono">{{ tdrLast.source }}</span>
        <span class="mx-2">•</span>
        {{ translate('troubleshooting.tdr_last_at', 'Last event') }}:
        <span class="font-mono">{{ tdrLastAtDisplay }}</span>
      </div>
    </n-alert>

    <div class="troubleshoot-grid">
      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.force_close') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{ $t('troubleshooting.force_close_desc') }}
            </p>
          </div>
          <n-button type="primary" strong :disabled="closeAppPressed" @click="closeApp">
            {{ $t('troubleshooting.force_close') }}
          </n-button>
        </div>
        <n-alert v-if="closeAppStatus === true" type="success" class="mt-3">
          {{ $t('troubleshooting.force_close_success') }}
        </n-alert>
        <n-alert v-else-if="closeAppStatus === false" type="error" class="mt-3">
          {{ $t('troubleshooting.force_close_error') }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.restart_sunshine') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{ $t('troubleshooting.restart_sunshine_desc') }}
            </p>
          </div>
          <n-button type="primary" strong :disabled="restartPressed" @click="restart">
            {{ $t('troubleshooting.restart_sunshine') }}
          </n-button>
        </div>
        <n-alert v-if="restartPressed === true" type="success" class="mt-3">
          {{ $t('troubleshooting.restart_sunshine_success') }}
        </n-alert>
      </section>

      <section v-if="platform === 'windows'" class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.collect_playnite_logs') || 'Export Logs' }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                $t('troubleshooting.collect_playnite_logs_desc') ||
                'Export LuminalShine, Playnite, plugin, and display-helper logs.'
              }}
            </p>
          </div>
          <n-button type="primary" strong @click="exportLogs">
            {{ $t('troubleshooting.collect_playnite_logs') || 'Export Logs' }}
          </n-button>
        </div>
      </section>

      <section v-if="platform === 'windows' && crashDumpAvailable" class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.export_crash_bundle') || 'Export Crash Bundle' }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                $t('troubleshooting.export_crash_bundle_desc') ||
                'Download logs and the most recent LuminalShine crash dump for issue reports.'
              }}
            </p>
          </div>
          <n-button
            type="error"
            strong
            :loading="exportCrashPending"
            :disabled="exportCrashPending"
            @click="exportCrashBundle"
          >
            {{
              exportCrashPending
                ? translate(
                    'troubleshooting.export_crash_bundle_preparing',
                    'Preparing Crash Bundle...',
                  )
                : translate('troubleshooting.export_crash_bundle', 'Export Crash Bundle')
            }}
          </n-button>
        </div>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div class="min-w-0">
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ translate('troubleshooting.tdr_card_title', 'GPU / Display Stack Health') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.tdr_card_desc',
                  'Tracks NVIDIA GPU TDR (Timeout Detection and Recovery) events and WDDM display-stack failures that LuminalShine detected. A TDR can end an active stream and momentarily wedge SudoVDA; the active session is refused while recovery is in progress so the client gets a quick failure instead of a frozen stream.',
                )
              }}
            </p>
            <div class="mt-2 flex flex-wrap items-center gap-2 text-xs">
              <span
                class="inline-flex items-center gap-1 px-2 py-0.5 rounded-md bg-dark/5 dark:bg-light/10"
              >
                <i class="fas fa-bolt" />
                {{ translate('troubleshooting.tdr_count_label', 'Events since process start') }}:
                <strong>{{ tdrCount }}</strong>
              </span>
              <span
                v-if="tdrRecoveryRecent"
                class="inline-flex items-center gap-1 px-2 py-0.5 rounded-md bg-warning/15 text-warning"
              >
                <i class="fas fa-triangle-exclamation" />
                {{
                  translate(
                    'troubleshooting.tdr_recovery_in_progress',
                    'Recovery in progress (new sessions refused)',
                  )
                }}
              </span>
            </div>
            <div v-if="tdrLast" class="mt-3 text-xs space-y-1">
              <div>
                <span class="opacity-70"
                  >{{ translate('troubleshooting.tdr_last_at', 'Last event') }}:</span
                >
                <span class="font-mono ml-1">{{ tdrLastAtDisplay }}</span>
              </div>
              <div>
                <span class="opacity-70"
                  >{{ translate('troubleshooting.tdr_last_source', 'Source') }}:</span
                >
                <span class="ml-1">{{ tdrLast.source }}</span>
              </div>
              <div v-if="tdrLast.hresult">
                <span class="opacity-70">HRESULT:</span>
                <span class="font-mono ml-1">0x{{ tdrLastHresultHex }}</span>
              </div>
              <div v-if="tdrLast.detail" class="opacity-80 break-words">
                {{ tdrLast.detail }}
              </div>
            </div>
            <p v-else class="mt-2 text-xs opacity-60 italic">
              {{ translate('troubleshooting.tdr_none', 'No TDR events recorded.') }}
            </p>
          </div>
          <n-button :loading="tdrRefreshing" :disabled="tdrRefreshing" @click="refreshTdrHealth">
            <i class="fas fa-rotate" />
            <span>{{ translate('troubleshooting.refresh', 'Refresh') }}</span>
          </n-button>
        </div>
      </section>

      <section v-if="platform === 'windows' && renderStackHasTip" class="troubleshoot-card">
        <div class="flex items-start gap-3">
          <i class="fas fa-circle-info text-primary mt-1" />
          <div class="min-w-0">
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ translate('troubleshooting.render_stack_title', 'Streaming environment') }}
            </h2>
            <p class="text-xs opacity-80 leading-snug mt-1">
              {{ renderStack?.tip }}
            </p>
            <div
              v-if="renderStackBadges.length"
              class="mt-2 flex flex-wrap items-center gap-1 text-xs"
            >
              <span
                v-for="badge in renderStackBadges"
                :key="badge"
                class="inline-flex items-center px-2 py-0.5 rounded-md bg-dark/5 dark:bg-light/10 font-mono"
              >
                {{ badge }}
              </span>
            </div>
          </div>
        </div>
      </section>

      <section v-if="platform === 'windows'" class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div class="min-w-0">
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ translate('troubleshooting.vdd_card_title', 'Virtual Display Driver') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.vdd_card_desc',
                  'Manual recovery for the SudoVDA virtual display driver. Use "Restart Virtual Display Driver" to PnP-disable and re-enable the SudoVDA root device (same as right-click Disable / Enable in Device Manager, but run as SYSTEM and scoped to SudoVDA only — other devices are not touched). Active streaming sessions will be torn down. Use "Show Diagnostic" to copy SudoVDA status into a support ticket without opening Device Manager.',
                )
              }}
            </p>
            <div v-if="vddDiag" class="mt-2 text-xs space-y-1">
              <div>
                <span class="opacity-70"
                  >{{ translate('troubleshooting.vdd_status', 'Status') }}:</span
                >
                <span class="ml-1">{{ vddDiag.status_string }}</span>
              </div>
              <div v-if="vddDiag.instance_id">
                <span class="opacity-70"
                  >{{ translate('troubleshooting.vdd_instance', 'Instance') }}:</span
                >
                <span class="font-mono ml-1 break-all">{{ vddDiag.instance_id }}</span>
              </div>
              <div v-if="vddDiag.problem_code">
                <span class="opacity-70">CM_PROB:</span>
                <span class="font-mono ml-1">{{ vddDiag.problem_code }}</span>
              </div>
            </div>
          </div>
          <div class="flex flex-col gap-2 shrink-0">
            <n-button
              type="primary"
              strong
              :loading="vddRestartPending"
              :disabled="vddRestartPending"
              @click="confirmRestartVdd"
            >
              <i class="fas fa-rotate-right" />
              <span>
                {{ translate('troubleshooting.vdd_restart', 'Restart Virtual Display Driver') }}
              </span>
            </n-button>
            <n-button
              :loading="vddDiagPending"
              :disabled="vddDiagPending"
              @click="showVddDiagnostic"
            >
              <i class="fas fa-circle-info" />
              <span>{{ translate('troubleshooting.vdd_show_diag', 'Show Diagnostic') }}</span>
            </n-button>
          </div>
        </div>
        <n-alert v-if="vddRestartStatus === 'success'" type="success" class="mt-3">
          {{ vddRestartMessage }}
        </n-alert>
        <n-alert v-else-if="vddRestartStatus === 'error'" type="error" class="mt-3">
          {{ vddRestartMessage }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ translate('troubleshooting.reset_admin_creds', 'Reset Admin Credentials') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.reset_admin_creds_desc',
                  'Removes the stored admin username and password from the system credential vault (Windows Credential Manager on Windows; the OS keyring on Linux / macOS). The next Web UI visit will prompt you to create a new admin user. Pairings and trusted devices are NOT affected.',
                )
              }}
            </p>
          </div>
          <n-button
            type="error"
            strong
            :loading="resetAdminCredsPending"
            :disabled="resetAdminCredsPending"
            @click="confirmResetAdminCreds"
          >
            {{
              resetAdminCredsPending
                ? translate('troubleshooting.reset_admin_creds_pending', 'Resetting...')
                : translate('troubleshooting.reset_admin_creds', 'Reset Admin Credentials')
            }}
          </n-button>
        </div>
        <n-alert v-if="resetAdminCredsStatus === 'success'" type="success" class="mt-3">
          {{
            resetAdminCredsMessage ||
            translate(
              'troubleshooting.reset_admin_creds_success',
              'Admin credentials cleared. Reload the page to create a new admin user.',
            )
          }}
        </n-alert>
        <n-alert v-else-if="resetAdminCredsStatus === 'error'" type="error" class="mt-3">
          {{ resetAdminCredsMessage }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{
                translate('troubleshooting.clear_steam_library', 'Clear Steam Library Cache')
              }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.clear_steam_library_desc',
                  'Deletes the steam_apps.json catalogue written by the Steam library auto-sync. The Moonlight app list reverts to just your hand-curated apps.json entries until the next sync tick (or until you reload apps.json). Toggling Steam auto-sync off in Settings hides these entries from Moonlight without deleting the file; this card deletes the file from disk.',
                )
              }}
            </p>
          </div>
          <n-button
            type="error"
            strong
            :loading="clearSteamLibraryPending"
            :disabled="clearSteamLibraryPending"
            @click="confirmClearSteamLibrary"
          >
            {{
              clearSteamLibraryPending
                ? translate('troubleshooting.clear_steam_library_pending', 'Clearing...')
                : translate('troubleshooting.clear_steam_library', 'Clear Steam Library Cache')
            }}
          </n-button>
        </div>
        <n-alert v-if="clearSteamLibraryStatus === 'success'" type="success" class="mt-3">
          {{
            clearSteamLibraryMessage ||
            translate(
              'troubleshooting.clear_steam_library_success',
              'Steam library cache cleared.',
            )
          }}
        </n-alert>
        <n-alert v-else-if="clearSteamLibraryStatus === 'error'" type="error" class="mt-3">
          {{ clearSteamLibraryMessage }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{
                translate(
                  'troubleshooting.clear_nonsteam_shortcuts',
                  'Clear Non-Steam Shortcuts Cache',
                )
              }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.clear_nonsteam_shortcuts_desc',
                  "Deletes the nonsg_apps.json catalogue written by the non-Steam shortcuts auto-sync. The Moonlight app list reverts to whatever your apps.json and (optionally) steam_apps.json produce. Toggling the shortcuts auto-sync off in Settings hides these entries from Moonlight without deleting the file; this card deletes the file from disk.",
                )
              }}
            </p>
          </div>
          <n-button
            type="error"
            strong
            :loading="clearNonsteamShortcutsPending"
            :disabled="clearNonsteamShortcutsPending"
            @click="confirmClearNonsteamShortcuts"
          >
            {{
              clearNonsteamShortcutsPending
                ? translate('troubleshooting.clear_nonsteam_shortcuts_pending', 'Clearing...')
                : translate(
                    'troubleshooting.clear_nonsteam_shortcuts',
                    'Clear Non-Steam Shortcuts Cache',
                  )
            }}
          </n-button>
        </div>
        <n-alert v-if="clearNonsteamShortcutsStatus === 'success'" type="success" class="mt-3">
          {{
            clearNonsteamShortcutsMessage ||
            translate(
              'troubleshooting.clear_nonsteam_shortcuts_success',
              'Non-Steam shortcuts cache cleared.',
            )
          }}
        </n-alert>
        <n-alert v-else-if="clearNonsteamShortcutsStatus === 'error'" type="error" class="mt-3">
          {{ clearNonsteamShortcutsMessage }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ translate('troubleshooting.clear_session_history', 'Clear Session History') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.clear_session_history_desc',
                  "Deletes every recorded session JSON in %ProgramData%\\LuminalShine\\sessions\\ . The Session History card on the Dashboard empties on the next 5-second refresh tick. The running LuminalShineSessionMonitor service keeps its in-memory ring buffer until it restarts; this card targets the on-disk archive specifically. Disable 'Record session telemetry' in Settings → Capture to stop future streams from populating new entries.",
                )
              }}
            </p>
          </div>
          <n-button
            type="error"
            strong
            :loading="clearSessionHistoryPending"
            :disabled="clearSessionHistoryPending"
            @click="confirmClearSessionHistory"
          >
            {{
              clearSessionHistoryPending
                ? translate('troubleshooting.clear_session_history_pending', 'Clearing...')
                : translate('troubleshooting.clear_session_history', 'Clear Session History')
            }}
          </n-button>
        </div>
        <n-alert v-if="clearSessionHistoryStatus === 'success'" type="success" class="mt-3">
          {{
            clearSessionHistoryMessage ||
            translate(
              'troubleshooting.clear_session_history_success',
              'Session history cleared.',
            )
          }}
        </n-alert>
        <n-alert v-else-if="clearSessionHistoryStatus === 'error'" type="error" class="mt-3">
          {{ clearSessionHistoryMessage }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ translate('troubleshooting.reset_state', 'Reset Stored Pairings') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                translate(
                  'troubleshooting.reset_state_desc',
                  'Recovery escape hatch when sunshine_state.json is corrupt or unreadable. The current state files are renamed with a .corrupt-TIMESTAMP suffix (kept on disk for forensics) and a fresh empty state is written. All paired clients will need to re-pair. Admin credentials are NOT touched.',
                )
              }}
            </p>
          </div>
          <n-button
            type="error"
            strong
            :loading="resetStatePending"
            :disabled="resetStatePending"
            @click="confirmResetState"
          >
            {{
              resetStatePending
                ? translate('troubleshooting.reset_state_pending', 'Resetting...')
                : translate('troubleshooting.reset_state', 'Reset Stored Pairings')
            }}
          </n-button>
        </div>
        <n-alert v-if="resetStateStatus === 'success'" type="success" class="mt-3">
          {{
            translate(
              'troubleshooting.reset_state_success',
              'State files reset. Re-pair Moonlight clients to resume streaming.',
            )
          }}
          <ul v-if="resetStateArchived.length" class="list-disc pl-5 mt-2 text-xs opacity-80">
            <li v-for="(p, i) in resetStateArchived" :key="i" class="font-mono break-all">
              {{ p }}
            </li>
          </ul>
        </n-alert>
        <n-alert v-else-if="resetStateStatus === 'error'" type="error" class="mt-3">
          {{
            resetStateError ||
            translate('troubleshooting.reset_state_error', 'Failed to reset stored state.')
          }}
        </n-alert>
      </section>
    </div>

    <section class="troubleshoot-card space-y-4">
      <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
        <div>
          <h2 class="text-base font-semibold text-dark dark:text-light">
            {{ $t('troubleshooting.logs') }}
          </h2>
          <p class="text-xs opacity-70 leading-snug">
            {{ $t('troubleshooting.logs_desc') }}
          </p>
        </div>
        <div class="flex flex-col sm:flex-row gap-2">
          <n-select
            v-if="logSourceOptions.length > 1"
            v-model:value="logSource"
            class="min-w-[200px]"
            :options="logSourceOptions"
            :placeholder="translate('troubleshooting.logs_source', 'Log source')"
          />
          <n-input v-model:value="logFilter" :placeholder="$t('troubleshooting.logs_find')" />
          <n-button
            type="primary"
            :aria-label="$t('troubleshooting.export_logs')"
            @click="exportLogs"
          >
            <i class="fas fa-download" />
            <span>{{ $t('troubleshooting.export_logs') }}</span>
          </n-button>
        </div>
      </div>

      <div
        v-if="rawSearchActive"
        class="flex flex-wrap items-center gap-2 text-xs text-dark/70 dark:text-light/70"
      >
        <span class="font-semibold text-dark/80 dark:text-light/80">
          {{ matchCountLabel }}
        </span>
        <n-button
          size="small"
          type="default"
          :disabled="matchCount === 0 || searchPending"
          @click="jumpToPreviousMatch"
        >
          {{ translate('troubleshooting.search_prev', 'Prev') }}
        </n-button>
        <n-button
          size="small"
          type="default"
          :disabled="matchCount === 0 || searchPending"
          @click="jumpToNextMatch"
        >
          {{ translate('troubleshooting.search_next', 'Next') }}
        </n-button>
        <n-button
          size="small"
          type="default"
          :disabled="logFilter.length === 0"
          @click="clearSearch"
        >
          {{ translate('troubleshooting.search_clear', 'Clear') }}
        </n-button>
        <span class="text-[11px] opacity-60">
          {{ searchContextLabel }}
        </span>
      </div>

      <div class="relative">
        <n-button
          v-if="newLogsAvailable && !rawSearchActive"
          class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full px-4 py-2 text-sm font-medium shadow-lg"
          type="primary"
          strong
          @click="jumpToLatest"
        >
          {{ $t('troubleshooting.new_logs_available') }}
          <span
            v-if="unseenLines > 0"
            class="ml-2 rounded bg-dark/10 dark:bg-light/10 px-2 py-0.5 text-xs"
          >
            +{{ unseenLines }}
          </span>
          <i class="fas fa-arrow-down ml-2" />
        </n-button>
        <n-button
          v-else-if="showJumpToLatest && !rawSearchActive"
          class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full px-4 py-2 text-sm font-medium shadow-lg"
          type="primary"
          strong
          @click="jumpToLatest"
        >
          {{ $t('troubleshooting.jump_to_latest') }}
          <i class="fas fa-arrow-down ml-2" />
        </n-button>

        <n-scrollbar
          ref="logScrollbar"
          style="height: 520px"
          class="border border-dark/10 dark:border-light/10 rounded-lg"
          @scroll="onLogScroll"
          @wheel="pauseAutoScroll"
          @mousedown="pauseAutoScroll"
          @touchstart="pauseAutoScroll"
        >
          <div
            class="m-0 bg-light dark:bg-dark font-mono text-[13px] leading-5 text-dark dark:text-light p-4 whitespace-pre-wrap break-words"
            @mousedown="pauseAutoScroll"
          >
            <div
              v-if="!searchActive"
              class="log-lines"
              :style="{ '--log-line-number-width': lineNumberWidth }"
            >
              <div
                v-for="(line, index) in logLines"
                :key="index"
                :ref="setLineRef(index)"
                class="log-line"
              >
                <span class="log-line-number">{{ index + 1 }}</span>
                <span class="log-line-text">{{ line.length === 0 ? ' ' : line }}</span>
              </div>
            </div>
            <div v-else class="space-y-2">
              <div
                class="flex items-center justify-between text-xs font-semibold text-dark/80 dark:text-light/80"
              >
                <span>{{ translate('troubleshooting.search_results', 'Results') }}</span>
                <span class="text-[11px] opacity-60">
                  {{ searchContextLabel }}
                  <template v-if="resultsRangeLabel"> | {{ resultsRangeLabel }}</template>
                </span>
              </div>
              <div
                v-if="searchInProgress"
                class="rounded-md border border-dark/10 dark:border-light/10 p-3 text-sm opacity-70"
              >
                {{ translate('troubleshooting.search_pending', 'Searching...') }}
              </div>
              <div
                v-else-if="matchCount === 0"
                class="rounded-md border border-dark/10 dark:border-light/10 p-3 text-sm opacity-70"
              >
                {{ translate('troubleshooting.search_no_matches', 'No matches') }}
              </div>
              <button
                v-for="result in searchResults"
                :key="result.id"
                type="button"
                class="w-full rounded-md border border-dark/10 dark:border-light/10 bg-white/80 dark:bg-surface/60 p-2 text-left transition hover:bg-dark/5 dark:hover:bg-light/5"
                :class="{
                  'border-amber-400/70 bg-amber-100/60 dark:bg-amber-500/10':
                    result.id === activeMatchIndex,
                }"
                :ref="setResultRef(result.id)"
                @click="openSearchResult(result.id)"
              >
                <div class="text-[11px] font-semibold text-dark/70 dark:text-light/70">
                  {{ translate('troubleshooting.search_line', 'Line') }} {{ result.lineIndex + 1 }}
                </div>
                <div
                  class="mt-1 font-mono text-[12px] leading-4 text-dark dark:text-light whitespace-pre-wrap break-words"
                  :style="{ '--log-line-number-width': lineNumberWidth }"
                >
                  <div
                    v-for="snippetLine in result.snippet"
                    :key="snippetLine.lineIndex"
                    class="log-line"
                  >
                    <span class="log-line-number">
                      {{ snippetLine.lineIndex + 1 }}
                    </span>
                    <span class="log-line-text">
                      <template
                        v-for="(segment, sIndex) in getLineSegments(
                          snippetLine.text,
                          snippetLine.lineIndex,
                        )"
                        :key="sIndex"
                      >
                        <span
                          :class="
                            segment.isMatch
                              ? snippetLine.lineIndex === activeLineIndex
                                ? 'log-match-active'
                                : 'log-match'
                              : ''
                          "
                          >{{ segment.text }}</span
                        >
                      </template>
                    </span>
                  </div>
                </div>
              </button>
            </div>
          </div>
        </n-scrollbar>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { h, ref, computed, onMounted, onBeforeUnmount, nextTick, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NInput, NAlert, NScrollbar, NSelect, useDialog } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import type { CrashDumpStatus } from '@/utils/crashDump';
import { isCrashDumpEligible, sanitizeCrashDumpStatus } from '@/utils/crashDump';

const store = useConfigStore();
const authStore = useAuthStore();
const { t } = useI18n();
const platform = computed(() => store.metadata.platform);

const crashDump = ref<CrashDumpStatus | null>(null);
const crashDumpAvailable = computed(() => isCrashDumpEligible(crashDump.value));
const exportCrashPending = ref(false);

const closeAppPressed = ref(false);
const closeAppStatus = ref(null as null | boolean);
const restartPressed = ref(false);
const resetStatePending = ref(false);
const resetStateStatus = ref<null | 'success' | 'error'>(null);
const resetStateError = ref('');
const resetStateArchived = ref<string[]>([]);
const resetAdminCredsPending = ref(false);
const resetAdminCredsStatus = ref<null | 'success' | 'error'>(null);
const resetAdminCredsMessage = ref('');
const clearSteamLibraryPending = ref(false);
const clearSteamLibraryStatus = ref<null | 'success' | 'error'>(null);
const clearSteamLibraryMessage = ref('');
const clearNonsteamShortcutsPending = ref(false);
const clearNonsteamShortcutsStatus = ref<null | 'success' | 'error'>(null);
const clearNonsteamShortcutsMessage = ref('');
const clearSessionHistoryPending = ref(false);
const clearSessionHistoryStatus = ref<null | 'success' | 'error'>(null);
const clearSessionHistoryMessage = ref('');
const dialog = useDialog();

type TdrLast = {
  at: number;
  source: string;
  hresult: number;
  detail: string;
};
const tdrCount = ref(0);
const tdrRecoveryRecent = ref(false);
const tdrLast = ref<TdrLast | null>(null);
const tdrRefreshing = ref(false);

const tdrLastAtDisplay = computed(() => {
  if (!tdrLast.value) return '';
  try {
    return new Date(tdrLast.value.at * 1000).toLocaleString();
  } catch {
    return String(tdrLast.value.at);
  }
});

const tdrLastHresultHex = computed(() => {
  if (!tdrLast.value || !tdrLast.value.hresult) return '';
  const v = tdrLast.value.hresult >>> 0;
  return v.toString(16).toUpperCase().padStart(8, '0');
});

async function refreshTdrHealth() {
  tdrRefreshing.value = true;
  try {
    const r = await http.get('./api/health/tdr', { validateStatus: () => true });
    const body = (r.data || {}) as Record<string, unknown>;
    if (r.status >= 200 && r.status < 300) {
      tdrCount.value = typeof body.count === 'number' ? body.count : 0;
      tdrRecoveryRecent.value = body.recovery_recent === true;
      const last = body.last as Partial<TdrLast> | undefined;
      if (last && typeof last.at === 'number' && typeof last.source === 'string') {
        tdrLast.value = {
          at: last.at,
          source: last.source,
          hresult: typeof last.hresult === 'number' ? last.hresult : 0,
          detail: typeof last.detail === 'string' ? last.detail : '',
        };
      } else {
        tdrLast.value = null;
      }
    }
  } catch {
    // surface as no-data rather than blocking the page
  } finally {
    tdrRefreshing.value = false;
  }
}

// Virtual Display Driver (SudoVDA) recovery card state.
type VddDiagnostic = {
  device_present: boolean;
  instance_id: string;
  hardware_ids: string;
  status_string: string;
  problem_code: number;
  last_recovery_at?: number;
  last_recovery_level: number;
  last_recovery_message: string;
};
const vddDiag = ref<VddDiagnostic | null>(null);
const vddDiagPending = ref(false);
const vddRestartPending = ref(false);
const vddRestartStatus = ref<null | 'success' | 'error'>(null);
const vddRestartMessage = ref('');

async function refreshVddDiagnostic() {
  if (platform.value !== 'windows') return;
  vddDiagPending.value = true;
  try {
    const r = await http.get('./api/state/vdd-diagnostic', { validateStatus: () => true });
    const body = (r.data || {}) as Partial<VddDiagnostic> & { status?: boolean };
    if (r.status >= 200 && r.status < 300 && body.status !== false) {
      vddDiag.value = {
        device_present: body.device_present === true,
        instance_id: typeof body.instance_id === 'string' ? body.instance_id : '',
        hardware_ids: typeof body.hardware_ids === 'string' ? body.hardware_ids : '',
        status_string: typeof body.status_string === 'string' ? body.status_string : 'Unknown',
        problem_code: typeof body.problem_code === 'number' ? body.problem_code : 0,
        last_recovery_at:
          typeof body.last_recovery_at === 'number' ? body.last_recovery_at : undefined,
        last_recovery_level:
          typeof body.last_recovery_level === 'number' ? body.last_recovery_level : 0,
        last_recovery_message:
          typeof body.last_recovery_message === 'string' ? body.last_recovery_message : '',
      };
    }
  } catch {
    // leave previous value in place
  } finally {
    vddDiagPending.value = false;
  }
}

function formatVddDiagnosticText(d: VddDiagnostic): string {
  const lines: string[] = [];
  lines.push('SudoVDA Virtual Display Driver diagnostic');
  lines.push('=========================================');
  lines.push(`Device present:  ${d.device_present ? 'yes' : 'no'}`);
  lines.push(`Status:          ${d.status_string}`);
  if (d.instance_id) lines.push(`Instance ID:     ${d.instance_id}`);
  if (d.hardware_ids) lines.push(`Hardware IDs:    ${d.hardware_ids}`);
  if (d.problem_code) lines.push(`CM_PROB code:    ${d.problem_code}`);
  if (d.last_recovery_at) {
    const ts = new Date(d.last_recovery_at * 1000).toLocaleString();
    const levelLabel =
      ['none', 'handle recycle', 'PnP restart'][d.last_recovery_level] ??
      String(d.last_recovery_level);
    lines.push(`Last recovery:   ${ts} (${levelLabel})`);
    if (d.last_recovery_message) {
      lines.push(`                 ${d.last_recovery_message}`);
    }
  }
  return lines.join('\n');
}

async function showVddDiagnostic() {
  // Always refresh first so the modal shows the latest state.
  await refreshVddDiagnostic();
  const d = vddDiag.value;
  if (!d) {
    dialog.error({
      title: translate('troubleshooting.vdd_diag_error', 'Could not collect diagnostic'),
      content: translate(
        'troubleshooting.vdd_diag_error_body',
        'The SudoVDA diagnostic endpoint did not return data. Check the LuminalShine log for details.',
      ),
      positiveText: translate('troubleshooting.close', 'Close'),
    });
    return;
  }
  const text = formatVddDiagnosticText(d);
  dialog.info({
    title: translate('troubleshooting.vdd_diag_title', 'SudoVDA Diagnostic'),
    content: () =>
      h(
        'pre',
        {
          class:
            'whitespace-pre-wrap break-all text-xs font-mono p-2 rounded bg-dark/5 dark:bg-light/10',
        },
        text,
      ),
    positiveText: translate('troubleshooting.copy', 'Copy to clipboard'),
    negativeText: translate('troubleshooting.close', 'Close'),
    onPositiveClick: () => {
      try {
        void navigator.clipboard.writeText(text);
      } catch {
        // best-effort
      }
    },
  });
}

async function doRestartVdd() {
  vddRestartPending.value = true;
  vddRestartStatus.value = null;
  vddRestartMessage.value = '';
  try {
    const r = await http.post('./api/state/vdd-restart', {}, { validateStatus: () => true });
    const body = (r.data || {}) as { status?: boolean; message?: string; level?: number };
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      vddRestartStatus.value = 'success';
      vddRestartMessage.value =
        typeof body.message === 'string' && body.message
          ? body.message
          : translate('troubleshooting.vdd_restart_success', 'Virtual display driver restarted.');
    } else {
      vddRestartStatus.value = 'error';
      vddRestartMessage.value =
        (typeof body.message === 'string' && body.message) || `HTTP ${r.status}`;
    }
  } catch (e: unknown) {
    vddRestartStatus.value = 'error';
    vddRestartMessage.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    vddRestartPending.value = false;
    // Refresh both the diagnostic snapshot and the TDR health card —
    // a successful restart should clear the recovery_recent flag.
    void refreshVddDiagnostic();
    void refreshTdrHealth();
  }
}

// Phase 0 — render-stack detection (DLSS / DLAA / FG) tip surface.
type RenderStackPayload = {
  status?: boolean;
  has_event: boolean;
  any_match?: boolean;
  has_dlss?: boolean;
  has_dlss_fg?: boolean;
  has_dlaa?: boolean;
  resolution_width?: number;
  resolution_height?: number;
  hdr_enabled?: boolean;
  bit_depth?: number;
  codec_label?: string;
  tip?: string;
};
const renderStack = ref<RenderStackPayload | null>(null);

const renderStackHasTip = computed(() => {
  const r = renderStack.value;
  return !!(r && r.has_event && r.tip && r.tip.length > 0);
});

const renderStackBadges = computed<string[]>(() => {
  const r = renderStack.value;
  if (!r || !r.has_event) return [];
  const badges: string[] = [];
  if (r.has_dlss_fg) badges.push('DLSS Frame Gen');
  else if (r.has_dlaa) badges.push('DLAA');
  else if (r.has_dlss) badges.push('DLSS');
  if (r.resolution_width && r.resolution_height) {
    badges.push(`${r.resolution_width}×${r.resolution_height}`);
  }
  if (r.hdr_enabled) badges.push('HDR');
  if (r.bit_depth) badges.push(`${r.bit_depth}-bit`);
  if (r.codec_label) badges.push(r.codec_label);
  return badges;
});

async function refreshRenderStack() {
  if (platform.value !== 'windows') return;
  try {
    const r = await http.get('./api/health/render-stack', { validateStatus: () => true });
    const body = (r.data || {}) as RenderStackPayload;
    if (r.status >= 200 && r.status < 300 && body.status !== false) {
      renderStack.value = body;
    }
  } catch {
    // leave previous value in place
  }
}

async function doResetAdminCreds() {
  resetAdminCredsPending.value = true;
  resetAdminCredsStatus.value = null;
  resetAdminCredsMessage.value = '';
  try {
    const r = await http.post(
      './api/state/reset-admin-credentials',
      {},
      { validateStatus: () => true },
    );
    const body = (r.data || {}) as { status?: boolean; message?: string; backend?: string };
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      resetAdminCredsStatus.value = 'success';
      resetAdminCredsMessage.value =
        (typeof body.message === 'string' && body.message) ||
        translate('troubleshooting.reset_admin_creds_success', 'Admin credentials cleared.');
    } else {
      resetAdminCredsStatus.value = 'error';
      resetAdminCredsMessage.value =
        (typeof body.message === 'string' && body.message) || `HTTP ${r.status}`;
    }
  } catch (e: unknown) {
    resetAdminCredsStatus.value = 'error';
    resetAdminCredsMessage.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    resetAdminCredsPending.value = false;
  }
}

function confirmResetAdminCreds() {
  if (resetAdminCredsPending.value) return;
  dialog.warning({
    title: translate('troubleshooting.reset_admin_creds_confirm_title', 'Reset Admin Credentials?'),
    content: translate(
      'troubleshooting.reset_admin_creds_confirm_body',
      'The stored admin username and password will be removed from the system credential vault. You will need to create a new admin user on the next Web UI visit. Paired Moonlight clients and trusted devices are NOT affected by this reset. Continue?',
    ),
    positiveText: translate('troubleshooting.reset_admin_creds_confirm_yes', 'Reset credentials'),
    negativeText: translate('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      await doResetAdminCreds();
    },
  });
}

async function doClearSteamLibrary() {
  clearSteamLibraryPending.value = true;
  clearSteamLibraryStatus.value = null;
  clearSteamLibraryMessage.value = '';
  try {
    const r = await http.post(
      './api/state/reset-steam-library-cache',
      {},
      { validateStatus: () => true },
    );
    const body = (r.data || {}) as { status?: boolean; message?: string };
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      clearSteamLibraryStatus.value = 'success';
      clearSteamLibraryMessage.value =
        (typeof body.message === 'string' && body.message) ||
        translate(
          'troubleshooting.clear_steam_library_success',
          'Steam library cache cleared.',
        );
    } else {
      clearSteamLibraryStatus.value = 'error';
      clearSteamLibraryMessage.value =
        (typeof body.message === 'string' && body.message) || `HTTP ${r.status}`;
    }
  } catch (e: unknown) {
    clearSteamLibraryStatus.value = 'error';
    clearSteamLibraryMessage.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    clearSteamLibraryPending.value = false;
  }
}

function confirmClearSteamLibrary() {
  if (clearSteamLibraryPending.value) return;
  dialog.warning({
    title: translate(
      'troubleshooting.clear_steam_library_confirm_title',
      'Clear Steam Library Cache?',
    ),
    content: translate(
      'troubleshooting.clear_steam_library_confirm_body',
      'The steam_apps.json catalogue will be deleted from disk and the Steam game entries will disappear from Moonlight. Your hand-curated apps.json (Desktop, Steam, anything you added by hand) is NOT touched. If the Steam auto-sync toggle is still ON, the catalogue will be re-generated on the next 30-second sync tick. Continue?',
    ),
    positiveText: translate(
      'troubleshooting.clear_steam_library_confirm_yes',
      'Clear cache',
    ),
    negativeText: translate('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      await doClearSteamLibrary();
    },
  });
}

async function doClearNonsteamShortcuts() {
  clearNonsteamShortcutsPending.value = true;
  clearNonsteamShortcutsStatus.value = null;
  clearNonsteamShortcutsMessage.value = '';
  try {
    const r = await http.post(
      './api/state/reset-nonsteam-shortcuts-cache',
      {},
      { validateStatus: () => true },
    );
    const body = (r.data || {}) as { status?: boolean; message?: string };
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      clearNonsteamShortcutsStatus.value = 'success';
      clearNonsteamShortcutsMessage.value =
        (typeof body.message === 'string' && body.message) ||
        translate(
          'troubleshooting.clear_nonsteam_shortcuts_success',
          'Non-Steam shortcuts cache cleared.',
        );
    } else {
      clearNonsteamShortcutsStatus.value = 'error';
      clearNonsteamShortcutsMessage.value =
        (typeof body.message === 'string' && body.message) || `HTTP ${r.status}`;
    }
  } catch (e: unknown) {
    clearNonsteamShortcutsStatus.value = 'error';
    clearNonsteamShortcutsMessage.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    clearNonsteamShortcutsPending.value = false;
  }
}

function confirmClearNonsteamShortcuts() {
  if (clearNonsteamShortcutsPending.value) return;
  dialog.warning({
    title: translate(
      'troubleshooting.clear_nonsteam_shortcuts_confirm_title',
      'Clear Non-Steam Shortcuts Cache?',
    ),
    content: translate(
      'troubleshooting.clear_nonsteam_shortcuts_confirm_body',
      'The nonsg_apps.json catalogue will be deleted from disk and the non-Steam shortcut entries will disappear from Moonlight. Your hand-curated apps.json (Desktop, Steam, anything you added by hand) and the Steam library catalogue are NOT touched. If the non-Steam shortcuts auto-sync toggle is still ON, the catalogue will be re-generated on the next 30-second sync tick. Continue?',
    ),
    positiveText: translate(
      'troubleshooting.clear_nonsteam_shortcuts_confirm_yes',
      'Clear cache',
    ),
    negativeText: translate('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      await doClearNonsteamShortcuts();
    },
  });
}

async function doClearSessionHistory() {
  clearSessionHistoryPending.value = true;
  clearSessionHistoryStatus.value = null;
  clearSessionHistoryMessage.value = '';
  try {
    const r = await http.post(
      './api/state/reset-session-history',
      {},
      { validateStatus: () => true },
    );
    const body = (r.data || {}) as { status?: boolean; message?: string };
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      clearSessionHistoryStatus.value = 'success';
      clearSessionHistoryMessage.value =
        (typeof body.message === 'string' && body.message) ||
        translate(
          'troubleshooting.clear_session_history_success',
          'Session history cleared.',
        );
    } else {
      clearSessionHistoryStatus.value = 'error';
      clearSessionHistoryMessage.value =
        (typeof body.message === 'string' && body.message) || `HTTP ${r.status}`;
    }
  } catch (e: unknown) {
    clearSessionHistoryStatus.value = 'error';
    clearSessionHistoryMessage.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    clearSessionHistoryPending.value = false;
  }
}

function confirmClearSessionHistory() {
  if (clearSessionHistoryPending.value) return;
  dialog.warning({
    title: translate(
      'troubleshooting.clear_session_history_confirm_title',
      'Clear Session History?',
    ),
    content: translate(
      'troubleshooting.clear_session_history_confirm_body',
      'Every recorded session JSON in %ProgramData%\\LuminalShine\\sessions\\ will be deleted and the Session History card on the Dashboard will empty within ~5 seconds. The running LuminalShineSessionMonitor service keeps its in-memory ring buffer until it restarts, so a currently-active stream still shows up live. Continue?',
    ),
    positiveText: translate(
      'troubleshooting.clear_session_history_confirm_yes',
      'Clear history',
    ),
    negativeText: translate('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      await doClearSessionHistory();
    },
  });
}

function confirmRestartVdd() {
  if (vddRestartPending.value) return;
  dialog.warning({
    title: translate(
      'troubleshooting.vdd_restart_confirm_title',
      'Restart Virtual Display Driver?',
    ),
    content: translate(
      'troubleshooting.vdd_restart_confirm_body',
      'This PnP-disables and re-enables the SudoVDA virtual display device. Any active streaming session using a virtual display will be ended; the Moonlight client will need to reconnect. Other devices are not affected.',
    ),
    positiveText: translate('troubleshooting.vdd_restart_yes', 'Restart driver'),
    negativeText: translate('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      await doRestartVdd();
    },
  });
}

const latestLogs = ref('Loading...');
const displayedLogs = ref('Loading...');
const logFilter = ref('');
const searchTerm = ref('');
const logSource = ref('sunshine');
type LogSegment = { text: string; isMatch: boolean };
const matchLines = ref<number[]>([]);
const matchCount = ref(0);
const searchInProgress = ref(false);
const searchResultLimit = 200;
const searchChunkSize = 1000;
let searchTaskId = 0;
let searchTaskTimer: number | null = null;
const segmentCache = new Map<number, { term: string; text: string; segments: LogSegment[] }>();

const translate = (key: string, fallback: string) => {
  const value = t(key);
  return value === key ? fallback : value;
};

const tCount = (key: string, fallback: string, count: number) => {
  const value = t(key, { count });
  return value === key ? fallback.replace('{count}', String(count)) : value;
};

const logSourceOptions = computed(() => {
  const options = [
    { label: translate('troubleshooting.logs_source_sunshine', 'LuminalShine'), value: 'sunshine' },
  ];
  if (platform.value === 'windows') {
    options.push(
      {
        label: translate('troubleshooting.logs_source_display_helper', 'Display helper'),
        value: 'display_helper',
      },
      {
        label: translate('troubleshooting.logs_source_playnite', 'Playnite'),
        value: 'playnite',
      },
      {
        label: translate('troubleshooting.logs_source_playnite_launcher', 'Playnite launcher'),
        value: 'playnite_launcher',
      },
      {
        label: translate('troubleshooting.logs_source_wgc', 'WGC helper'),
        value: 'wgc',
      },
    );
  }
  return options;
});

const logScrollbar = ref<InstanceType<typeof NScrollbar> | null>(null);
const autoScrollEnabled = ref(true);
const latestLineCount = ref(0);
const displayedLineCount = ref(0);
const isAtBottom = ref(true);

let logInterval: number | null = null;
let loginDisposer: (() => void) | null = null;
let searchDebounce: number | null = null;

const lineRefs = new Map<number, HTMLElement>();
const resultRefs = new Map<number, HTMLElement>();
const pendingJumpLine = ref<number | null>(null);

const setLineRef = (index: number) => (el: HTMLElement | null) => {
  if (el) {
    lineRefs.set(index, el);
  } else {
    lineRefs.delete(index);
  }
};

const setResultRef = (index: number) => (el: HTMLElement | null) => {
  if (el) {
    resultRefs.set(index, el);
  } else {
    resultRefs.delete(index);
  }
};

const rawSearch = computed(() => logFilter.value.trim());
const rawSearchActive = computed(() => rawSearch.value.length > 0);
const searchActive = computed(() => searchTerm.value.length > 0);
const logLines = computed(() => (displayedLogs.value ?? '').split('\n'));
const logLinesLower = computed(() => logLines.value.map((line) => line.toLowerCase()));
const lineNumberWidth = computed(() => {
  const digits = Math.max(3, String(logLines.value.length || 0).length);
  return `${digits}ch`;
});
const contextLines = 5;
const activeMatchIndex = ref(-1);
const activeLineIndex = computed(() =>
  activeMatchIndex.value >= 0 ? (matchLines.value[activeMatchIndex.value] ?? null) : null,
);

const searchPending = computed(
  () => rawSearchActive.value && (rawSearch.value !== searchTerm.value || searchInProgress.value),
);
const matchCountLabel = computed(() => {
  if (!rawSearchActive.value) return '';
  if (searchPending.value) {
    return translate('troubleshooting.search_pending', 'Searching...');
  }
  if (matchCount.value === 0) {
    return translate('troubleshooting.search_no_matches', 'No matches');
  }
  return tCount('troubleshooting.search_matches', '{count} matches', matchCount.value);
});
const searchContextLabel = computed(() =>
  tCount('troubleshooting.search_context', '{count} lines of context', contextLines),
);

const resultsWindow = computed(() => {
  const total = matchLines.value.length;
  if (total <= searchResultLimit) {
    return { start: 0, end: total };
  }
  let start = 0;
  if (activeMatchIndex.value >= 0) {
    const half = Math.floor(searchResultLimit / 2);
    start = Math.max(
      0,
      Math.min(activeMatchIndex.value - half, Math.max(0, total - searchResultLimit)),
    );
  }
  return { start, end: Math.min(total, start + searchResultLimit) };
});

const resultsRangeLabel = computed(() => {
  const total = matchLines.value.length;
  if (total <= searchResultLimit) return '';
  const { start, end } = resultsWindow.value;
  const translated = t('troubleshooting.search_results_window', {
    start: start + 1,
    end,
    count: total,
  });
  if (translated === 'troubleshooting.search_results_window') {
    return `Showing ${start + 1}-${end} of ${total} results`;
  }
  return translated;
});

const searchResults = computed(() => {
  if (!searchActive.value || searchInProgress.value) return [];
  const lines = logLines.value;
  const { start: windowStart, end: windowEnd } = resultsWindow.value;
  return matchLines.value.slice(windowStart, windowEnd).map((lineIndex, offset) => {
    const id = windowStart + offset;
    const snippetStart = Math.max(0, lineIndex - contextLines);
    const snippetEnd = Math.min(lines.length - 1, lineIndex + contextLines);
    const snippet = [];
    for (let i = snippetStart; i <= snippetEnd; i += 1) {
      snippet.push({ lineIndex: i, text: lines[i] ?? '' });
    }
    return { id, lineIndex, snippet };
  });
});

function getLineSegments(line: string, lineIndex: number) {
  const term = searchTerm.value.trim();
  if (!term) {
    return [{ text: line.length === 0 ? ' ' : line, isMatch: false }];
  }
  const cached = segmentCache.get(lineIndex);
  if (cached && cached.term === term && cached.text === line) {
    return cached.segments;
  }
  const needle = term.toLowerCase();
  const lower = line.toLowerCase();
  const segments: LogSegment[] = [];
  let cursor = 0;
  let matchIndex = lower.indexOf(needle, cursor);
  while (matchIndex !== -1) {
    if (matchIndex > cursor) {
      segments.push({ text: line.slice(cursor, matchIndex), isMatch: false });
    }
    segments.push({
      text: line.slice(matchIndex, matchIndex + needle.length),
      isMatch: true,
    });
    cursor = matchIndex + needle.length;
    matchIndex = lower.indexOf(needle, cursor);
  }
  if (cursor < line.length) {
    segments.push({ text: line.slice(cursor), isMatch: false });
  }
  if (segments.length === 0) {
    segments.push({ text: line.length === 0 ? ' ' : line, isMatch: false });
  }
  segmentCache.set(lineIndex, { term, text: line, segments });
  return segments;
}
const unseenLines = computed(() => Math.max(0, latestLineCount.value - displayedLineCount.value));
const newLogsAvailable = computed(() => unseenLines.value > 0);
const showJumpToLatest = computed(
  () => !newLogsAvailable.value && !isAtBottom.value && !autoScrollEnabled.value,
);

function resetLogState() {
  latestLogs.value = 'Loading...';
  displayedLogs.value = 'Loading...';
  latestLineCount.value = 0;
  displayedLineCount.value = 0;
  autoScrollEnabled.value = true;
  isAtBottom.value = true;
}

function buildLogUrl() {
  if (logSource.value === 'sunshine') return './api/logs';
  const params = new URLSearchParams();
  params.set('source', logSource.value);
  return `./api/logs?${params.toString()}`;
}

function getLogContainer() {
  // Naive UI's <n-scrollbar> exposes `scrollbarInstRef` (a Vue ref) which is sometimes
  // auto-unwrapped by the component public instance proxy. Handle both shapes.
  const maybe = (logScrollbar.value as any)?.scrollbarInstRef;
  const internal = maybe && typeof maybe === 'object' && 'value' in maybe ? maybe.value : maybe;
  const fromInst = internal?.containerRef ?? null;
  if (fromInst) return fromInst;

  // Fallback: query the DOM in case the internal instance shape changes.
  const rootEl = (logScrollbar.value as any)?.$el as HTMLElement | undefined;
  if (!rootEl) return null;
  return (
    rootEl.querySelector<HTMLElement>('.n-scrollbar-container') ??
    rootEl.querySelector<HTMLElement>('[class*="-scrollbar-container"]') ??
    null
  );
}

function hasActiveLogSelection() {
  if (typeof window === 'undefined') return false;
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed) return false;
  const container = getLogContainer();
  if (!container) return false;
  const anchor = selection.anchorNode;
  const focus = selection.focusNode;
  return !!anchor && !!focus && container.contains(anchor) && container.contains(focus);
}

function onLogScroll() {
  const container = getLogContainer();
  if (!container) return;
  const atBottom = isNearBottom(container);
  isAtBottom.value = atBottom;
  if (atBottom) {
    if (!rawSearchActive.value) {
      autoScrollEnabled.value = true;
      displayedLogs.value = latestLogs.value;
      displayedLineCount.value = latestLineCount.value;
      scrollToBottom();
    } else {
      autoScrollEnabled.value = false;
    }
  } else {
    autoScrollEnabled.value = false;
  }
}

function isNearBottom(el: HTMLElement) {
  const threshold = 24;
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

function scrollToBottom() {
  const doScroll = () => {
    // Avoid relying on scrollHeight math (and keep types happy).
    // Naive UI's internal scrollbar clamps large values to the bottom.
    logScrollbar.value?.scrollTo({ top: Number.MAX_SAFE_INTEGER, behavior: 'auto' });

    // Fallback for cases where the component method no-ops (e.g., container not ready yet).
    const container = getLogContainer();
    if (!container) return;
    container.scrollTop = container.scrollHeight;
    container.scrollTo?.({ top: container.scrollHeight, behavior: 'auto' });
    isAtBottom.value = isNearBottom(container);
  };

  doScroll();
  if (typeof window !== 'undefined') {
    window.requestAnimationFrame(() => doScroll());
  }
}

function scrollToResult(index: number) {
  const resultEl = resultRefs.get(index);
  if (resultEl?.scrollIntoView) {
    resultEl.scrollIntoView({ block: 'center' });
  }
}

function scrollToLogLine(lineIndex: number) {
  const lineEl = lineRefs.get(lineIndex);
  if (!lineEl) return;
  lineEl.scrollIntoView({ block: 'center' });
  flashLogLine(lineEl);
  const container = getLogContainer();
  if (container) isAtBottom.value = isNearBottom(container);
}

function flashLogLine(lineEl: HTMLElement) {
  lineEl.classList.remove('log-flash');
  // Force reflow to restart animation on rapid repeats.
  void lineEl.offsetWidth;
  lineEl.classList.add('log-flash');
  if (typeof window !== 'undefined') {
    window.setTimeout(() => lineEl.classList.remove('log-flash'), 3000);
  }
}

function pauseAutoScroll() {
  if (!autoScrollEnabled.value) return;
  autoScrollEnabled.value = false;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
}

function setActiveMatch(index: number) {
  if (matchLines.value.length === 0) return;
  const total = matchLines.value.length;
  const nextIndex = ((index % total) + total) % total;
  activeMatchIndex.value = nextIndex;
  autoScrollEnabled.value = false;
  nextTick(() => {
    scrollToResult(nextIndex);
  });
}

function openSearchResult(index: number) {
  const lineIndex = matchLines.value[index];
  if (lineIndex === undefined) return;
  pendingJumpLine.value = lineIndex;
  activeMatchIndex.value = index;
  autoScrollEnabled.value = false;
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  cancelSearchTask();
  logFilter.value = '';
  searchTerm.value = '';
}

function jumpToPreviousMatch() {
  if (matchLines.value.length === 0) return;
  setActiveMatch(activeMatchIndex.value - 1);
}

function jumpToNextMatch() {
  if (matchLines.value.length === 0) return;
  setActiveMatch(activeMatchIndex.value + 1);
}

function clearSearch() {
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  cancelSearchTask();
  pendingJumpLine.value = null;
  logFilter.value = '';
  searchTerm.value = '';
}

function cancelSearchTask() {
  searchTaskId += 1;
  searchInProgress.value = false;
  if (searchTaskTimer !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchTaskTimer);
    searchTaskTimer = null;
  }
}

function startSearch(term: string) {
  cancelSearchTask();
  segmentCache.clear();
  matchLines.value = [];
  matchCount.value = 0;
  const trimmed = term.trim();
  if (!trimmed) return;
  const needle = trimmed.toLowerCase();
  if (!needle) return;

  const linesLower = logLinesLower.value;
  const totalLines = linesLower.length;
  let index = 0;
  let count = 0;
  const matches: number[] = [];
  const jobId = searchTaskId;

  if (typeof window === 'undefined') {
    for (index = 0; index < totalLines; index += 1) {
      const lower = linesLower[index] ?? '';
      let fromIndex = 0;
      let lineHasMatch = false;
      while (fromIndex <= lower.length) {
        const matchIndex = lower.indexOf(needle, fromIndex);
        if (matchIndex === -1) break;
        count += 1;
        lineHasMatch = true;
        fromIndex = matchIndex + needle.length;
      }
      if (lineHasMatch) matches.push(index);
    }
    matchLines.value = matches;
    matchCount.value = count;
    searchInProgress.value = false;
    return;
  }

  const processChunk = () => {
    if (jobId !== searchTaskId) return;
    const end = Math.min(totalLines, index + searchChunkSize);
    for (; index < end; index += 1) {
      const lower = linesLower[index] ?? '';
      let fromIndex = 0;
      let lineHasMatch = false;
      while (fromIndex <= lower.length) {
        const matchIndex = lower.indexOf(needle, fromIndex);
        if (matchIndex === -1) break;
        count += 1;
        lineHasMatch = true;
        fromIndex = matchIndex + needle.length;
      }
      if (lineHasMatch) matches.push(index);
    }
    if (index < totalLines) {
      searchTaskTimer = window.setTimeout(processChunk, 0);
      return;
    }
    searchTaskTimer = null;
    if (jobId !== searchTaskId) return;
    matchLines.value = matches;
    matchCount.value = count;
    searchInProgress.value = false;
  };

  searchInProgress.value = true;
  searchTaskTimer = window.setTimeout(processChunk, 0);
}

async function refreshLogs() {
  if (!authStore.isAuthenticated) return;
  if (authStore.loggingIn && authStore.loggingIn.value) return;

  try {
    const r = await http.get(buildLogUrl(), {
      responseType: 'text',
      transformResponse: [(v) => v],
    });
    if (r.status !== 200 || typeof r.data !== 'string') return;
    const nextText = r.data;

    latestLogs.value = nextText;

    const nextLines = nextText ? nextText.split('\n') : [];
    latestLineCount.value = nextLines.length;

    const selectionActive = hasActiveLogSelection();
    const searchActiveNow = searchActive.value;
    const searchInputActive = rawSearchActive.value;
    const container = getLogContainer();
    const atBottom = container ? isNearBottom(container) : true;
    isAtBottom.value = atBottom;
    if (searchInputActive) {
      autoScrollEnabled.value = false;
    } else if (!atBottom && autoScrollEnabled.value) {
      autoScrollEnabled.value = false;
    }
    const shouldAutoScroll =
      autoScrollEnabled.value && atBottom && !selectionActive && !searchActiveNow;

    if (shouldAutoScroll) {
      displayedLogs.value = nextText;
      displayedLineCount.value = latestLineCount.value;
      await nextTick();
      scrollToBottom();
    } else {
      if (latestLineCount.value < displayedLineCount.value) {
        displayedLogs.value = nextText;
        displayedLineCount.value = latestLineCount.value;
      }
    }
  } catch {
    // ignore errors
  }
}

function exportLogs() {
  try {
    if (typeof window === 'undefined') return;
    if (platform.value === 'windows') {
      window.location.href = './api/logs/export';
      return;
    }
    const content = latestLogs.value || displayedLogs.value || '';
    const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
    const url = window.URL.createObjectURL(blob);
    const link = window.document.createElement('a');
    const timestamp = new Date()
      .toISOString()
      .replace(/[:.]/g, '-')
      .replace('T', '_')
      .replace('Z', '');
    link.href = url;
    link.download = `sunshine-logs-${timestamp}.log`;
    link.click();
    window.URL.revokeObjectURL(url);
  } catch (_) {}
}

async function refreshCrashDumpStatus() {
  try {
    if (platform.value === 'windows') {
      const r = await http.get('/api/health/crashdump', { validateStatus: () => true });
      if (r.status === 200 && r.data) {
        const sanitized = sanitizeCrashDumpStatus(r.data as CrashDumpStatus);
        crashDump.value = sanitized ?? { available: false };
      } else {
        crashDump.value = { available: false };
      }
    } else {
      crashDump.value = null;
    }
  } catch {
    crashDump.value = null;
  }
}

function exportCrashBundle() {
  return void exportCrashBundleAsync();
}

function parseContentDispositionFilename(header?: string): string | null {
  if (!header) return null;
  const filenameStar = /filename\*=UTF-8''([^;]+)/i.exec(header);
  if (filenameStar?.[1]) {
    try {
      return decodeURIComponent(filenameStar[1]);
    } catch {
      return filenameStar[1];
    }
  }
  const filenameMatch = /filename="?([^\";]+)"?/i.exec(header);
  return filenameMatch?.[1] || null;
}

function triggerDownload(blob: Blob, filename: string) {
  const url = window.URL.createObjectURL(blob);
  const link = window.document.createElement('a');
  link.href = url;
  link.download = filename;
  link.click();
  window.URL.revokeObjectURL(url);
}

async function downloadCrashBundlePart(partIndex: number, filenameHint?: string) {
  const r = await http.get(`/api/logs/export_crash?part=${partIndex}`, {
    responseType: 'blob',
    validateStatus: () => true,
  });
  if (r.status !== 200) {
    throw new Error('crash bundle download failed');
  }
  const headerName = parseContentDispositionFilename(r.headers?.['content-disposition']);
  const filename = filenameHint || headerName || `sunshine_crashbundle-part${partIndex}.zip`;
  triggerDownload(r.data as Blob, filename);
}

async function exportCrashBundleAsync() {
  if (exportCrashPending.value) return;
  exportCrashPending.value = true;
  try {
    if (typeof window === 'undefined') return;
    const manifest = await http.get('/api/logs/export_crash/manifest', {
      validateStatus: () => true,
    });
    const parts = Array.isArray(manifest.data?.parts) ? manifest.data.parts : [];
    if (manifest.status === 200 && parts.length > 0) {
      const ordered = [...parts].sort((a, b) => Number(a.index) - Number(b.index));
      for (const part of ordered) {
        const index = Number(part.index) || 0;
        if (index <= 0) continue;
        await downloadCrashBundlePart(index, part.filename);
      }
    } else {
      await downloadCrashBundlePart(1);
    }
  } catch {
    // ignore errors
  } finally {
    exportCrashPending.value = false;
  }
}

function jumpToLatest() {
  autoScrollEnabled.value = true;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
  nextTick(() => {
    scrollToBottom();
  });
}

async function closeApp() {
  closeAppPressed.value = true;
  try {
    const r = await http.post('./api/apps/close', {}, { validateStatus: () => true });
    closeAppStatus.value = r.data?.status === true;
  } catch {
    closeAppStatus.value = false;
  } finally {
    closeAppPressed.value = false;
    setTimeout(() => (closeAppStatus.value = null), 5000);
  }
}

function restart() {
  restartPressed.value = true;
  setTimeout(() => (restartPressed.value = false), 5000);
  http.post('./api/restart', {}, { validateStatus: () => true });
}

async function doResetState() {
  resetStatePending.value = true;
  resetStateStatus.value = null;
  resetStateError.value = '';
  resetStateArchived.value = [];
  try {
    const r = await http.post('./api/state/reset', {}, { validateStatus: () => true });
    const body = r.data || {};
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      resetStateStatus.value = 'success';
      const archived = Array.isArray(body.archived) ? body.archived : [];
      resetStateArchived.value = archived.filter(
        (p: unknown): p is string => typeof p === 'string',
      );
    } else {
      resetStateStatus.value = 'error';
      resetStateError.value =
        (body && typeof body.error === 'string' && body.error) || `HTTP ${r.status}`;
    }
  } catch (e: unknown) {
    resetStateStatus.value = 'error';
    resetStateError.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    resetStatePending.value = false;
  }
}

function confirmResetState() {
  if (resetStatePending.value) return;
  dialog.warning({
    title: translate('troubleshooting.reset_state_confirm_title', 'Reset stored pairings?'),
    content: translate(
      'troubleshooting.reset_state_confirm_body',
      'All paired Moonlight clients will need to re-pair. Current sunshine_state.json and luminalshine_state.json files will be renamed with a .corrupt-TIMESTAMP suffix (kept on disk for forensics) and a fresh state will be written. Admin credentials are NOT touched. Continue?',
    ),
    positiveText: translate('troubleshooting.reset_state_confirm_yes', 'Reset stored state'),
    negativeText: translate('troubleshooting.reset_state_confirm_no', 'Cancel'),
    onPositiveClick: async () => {
      await doResetState();
    },
  });
}

onMounted(async () => {
  loginDisposer = authStore.onLogin(() => {
    void refreshLogs();
    void refreshCrashDumpStatus();
    void refreshTdrHealth();
    void refreshVddDiagnostic();
    void refreshRenderStack();
  });

  await authStore.waitForAuthentication();

  await refreshCrashDumpStatus();
  await refreshTdrHealth();
  await refreshVddDiagnostic();
  await refreshRenderStack();

  nextTick(() => {
    if (getLogContainer()) scrollToBottom();
  });

  logInterval = window.setInterval(refreshLogs, 5000);
  refreshLogs();
});

onBeforeUnmount(() => {
  if (logInterval) window.clearInterval(logInterval);
  if (loginDisposer) loginDisposer();
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  cancelSearchTask();
});

watch(rawSearch, (value) => {
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  if (!value) {
    searchTerm.value = '';
    activeMatchIndex.value = -1;
    return;
  }
  autoScrollEnabled.value = false;
  if (typeof window === 'undefined') {
    searchTerm.value = value;
    return;
  }
  searchDebounce = window.setTimeout(() => {
    searchTerm.value = value;
  }, 150);
});

watch([searchTerm, logLinesLower], ([term]) => {
  startSearch(term);
});

watch(searchActive, (active) => {
  if (active) {
    autoScrollEnabled.value = false;
    return;
  }
  activeMatchIndex.value = -1;
  if (pendingJumpLine.value !== null) {
    const targetLine = pendingJumpLine.value;
    pendingJumpLine.value = null;
    nextTick(() => {
      scrollToLogLine(targetLine);
    });
    return;
  }
  const container = getLogContainer();
  if (container && isNearBottom(container)) {
    autoScrollEnabled.value = true;
    displayedLogs.value = latestLogs.value;
    displayedLineCount.value = latestLineCount.value;
    nextTick(() => scrollToBottom());
  }
});

watch(matchLines, (list) => {
  if (!searchActive.value || list.length === 0) {
    activeMatchIndex.value = -1;
    return;
  }
  if (activeMatchIndex.value >= list.length) {
    activeMatchIndex.value = list.length - 1;
  }
});

watch(searchTerm, (value, oldValue) => {
  if (value !== oldValue) {
    activeMatchIndex.value = -1;
  }
});

watch(logSource, () => {
  resetLogState();
  void refreshLogs();
  nextTick(() => scrollToBottom());
});
</script>

<style scoped>
.troubleshoot-root {
  @apply space-y-6;
}

.troubleshoot-grid {
  @apply grid grid-cols-1 lg:grid-cols-2 gap-4;
}

.troubleshoot-card {
  @apply rounded-2xl border border-dark/10 dark:border-light/10 bg-white/90 dark:bg-surface/80 shadow-sm px-5 py-4 space-y-3;
}

.log-lines {
  @apply space-y-0;
}

.log-line {
  display: grid;
  grid-template-columns: var(--log-line-number-width, 4ch) minmax(0, 1fr);
  column-gap: 0.75rem;
  align-items: start;
  padding: 0.25rem 0;
}

.log-line-number {
  @apply text-left opacity-50 tabular-nums font-mono;
  user-select: none;
}

.log-line-text {
  @apply min-w-0 break-words;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

.log-match {
  @apply rounded-sm bg-amber-200/70 dark:bg-amber-400/30;
}

.log-match-active {
  @apply rounded-sm bg-amber-400/80 dark:bg-amber-500/50;
}

.log-flash {
  border-radius: 6px;
  box-shadow:
    0 0 0 3px rgb(var(--color-secondary) / 0.5),
    0 0 0 6px rgb(var(--color-secondary) / 0.25);
  outline: 2px solid rgb(var(--color-secondary) / 0.55);
  outline-offset: 2px;
  animation: log-flash-fade 3s ease-out forwards;
}

:deep(.n-scrollbar-container) {
  overflow-x: hidden !important;
}

:deep(.n-scrollbar-rail--horizontal) {
  display: none !important;
}

@keyframes log-flash-fade {
  0% {
    box-shadow:
      0 0 0 3px rgb(var(--color-secondary) / 0.55),
      0 0 0 6px rgb(var(--color-secondary) / 0.28);
    outline-color: rgb(var(--color-secondary) / 0.65);
  }
  70% {
    box-shadow:
      0 0 0 3px rgb(var(--color-secondary) / 0.25),
      0 0 0 6px rgb(var(--color-secondary) / 0.12);
    outline-color: rgb(var(--color-secondary) / 0.35);
  }
  100% {
    box-shadow:
      0 0 0 3px rgb(var(--color-secondary) / 0),
      0 0 0 6px rgb(var(--color-secondary) / 0);
    outline-color: rgb(var(--color-secondary) / 0);
  }
}

.dark .log-flash {
  animation-name: log-flash-fade-dark;
}

@keyframes log-flash-fade-dark {
  0% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0.45),
      0 0 0 6px rgb(var(--color-primary) / 0.22);
    outline-color: rgb(var(--color-primary) / 0.55);
  }
  70% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0.2),
      0 0 0 6px rgb(var(--color-primary) / 0.1);
    outline-color: rgb(var(--color-primary) / 0.3);
  }
  100% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0),
      0 0 0 6px rgb(var(--color-primary) / 0);
    outline-color: rgb(var(--color-primary) / 0);
  }
}
</style>
