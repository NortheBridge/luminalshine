// Renders GitHub-flavored Markdown (typically the body of a GitHub release)
// into sanitized HTML for display in the Vue Dashboard's release-notes panes.
//
// Pipeline:
//   marked (GFM, headings, lists, tables, task lists, strikethrough)
//     -> marked-highlight  (fenced code blocks colorized via highlight.js)
//     -> custom inline extensions for @username and #issue autolinks
//     -> DOMPurify         (strips scripts, event handlers, javascript: URLs)
//     -> string handed to v-html
//
// All links open in a new tab with rel="noopener noreferrer".

import { marked } from 'marked';
import { markedHighlight } from 'marked-highlight';
import hljs from 'highlight.js';
import DOMPurify from 'dompurify';
// One stylesheet for code-block syntax colors. github-dark.css matches GitHub's
// dark-mode rendering; we apply it unconditionally because the release-notes
// container has a dark backdrop in both light and dark site themes.
import 'highlight.js/styles/github-dark.css';

const REPO_OWNER = 'NortheBridge';
const REPO_NAME = 'luminalshine';

// --- highlight.js integration ----------------------------------------------
// markedHighlight runs once per fenced code block. We honor an explicit
// language tag (```ts) when valid, otherwise we let hljs auto-detect.
marked.use(
  markedHighlight({
    langPrefix: 'hljs language-',
    highlight(code, lang) {
      const language = lang && hljs.getLanguage(lang) ? lang : 'plaintext';
      try {
        return hljs.highlight(code, { language, ignoreIllegals: true }).value;
      } catch {
        return code;
      }
    },
  }),
);

// --- GFM + base options ----------------------------------------------------
marked.use({
  gfm: true,
  breaks: false, // GitHub release notes use blank lines, not soft breaks
});

// --- Inline autolink extensions: @username and #issue ----------------------
// These run as inline tokenizers, so they never fire inside fenced code
// blocks (marked doesn't parse inline content there). The lookbehind in
// `start` keeps the matcher from triggering on email addresses, URL
// fragments, and `org/repo#123` cross-repo refs.
type MentionToken = { type: 'mention'; raw: string; username: string };
type IssueRefToken = { type: 'issueRef'; raw: string; issue: string };

marked.use({
  extensions: [
    {
      name: 'mention',
      level: 'inline',
      start(src: string) {
        const m = src.match(/(?<![\w@./-])@[a-zA-Z0-9]/);
        return m?.index;
      },
      tokenizer(src: string): MentionToken | undefined {
        const match = /^@([a-zA-Z0-9][\w-]*)/.exec(src);
        if (!match) return undefined;
        return { type: 'mention', raw: match[0], username: match[1] };
      },
      renderer(token: any) {
        const t = token as MentionToken;
        return `<a href="https://github.com/${t.username}" target="_blank" rel="noopener noreferrer">@${t.username}</a>`;
      },
    },
    {
      name: 'issueRef',
      level: 'inline',
      start(src: string) {
        const m = src.match(/(?<![\w#/])#\d/);
        return m?.index;
      },
      tokenizer(src: string): IssueRefToken | undefined {
        const match = /^#(\d+)\b/.exec(src);
        if (!match) return undefined;
        return { type: 'issueRef', raw: match[0], issue: match[1] };
      },
      renderer(token: any) {
        const t = token as IssueRefToken;
        return `<a href="https://github.com/${REPO_OWNER}/${REPO_NAME}/issues/${t.issue}" target="_blank" rel="noopener noreferrer">#${t.issue}</a>`;
      },
    },
  ],
});

// --- DOMPurify hook: force target=_blank + rel=noopener on every link -----
// Release-note links should always open in a new tab so the user doesn't
// lose the Dashboard. addHook is module-global; this file is imported once
// and the hook stays for the lifetime of the SPA. Adding it inside the
// render function would re-register on every call.
let hookInstalled = false;
function ensureLinkTargetHook() {
  if (hookInstalled) return;
  DOMPurify.addHook('afterSanitizeAttributes', (node: Element) => {
    if (node.tagName === 'A') {
      node.setAttribute('target', '_blank');
      node.setAttribute('rel', 'noopener noreferrer');
    }
  });
  hookInstalled = true;
}

/**
 * Parse `body` as GitHub-flavored Markdown and return sanitized HTML safe to
 * feed into v-html. Returns an empty string for null/undefined input.
 */
export function renderReleaseMarkdown(body: string | null | undefined): string {
  if (!body) return '';
  ensureLinkTargetHook();
  // marked.parse with no async option returns a string in v14 sync mode.
  const rawHtml = marked.parse(body, { async: false }) as string;
  return DOMPurify.sanitize(rawHtml, {
    USE_PROFILES: { html: true },
    ADD_ATTR: ['target', 'rel'],
  });
}
