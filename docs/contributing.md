# Contributing
Read our contribution guide at the organization level


## Recommended Tools

| Tool                                                                                                                                                                           | Description                                                             |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------|
| <a href="https://code.visualstudio.com/"><svg width="16" height="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg" fill="currentColor"><path d="M15.434 1.72887L12.14 0.144875C12.002 0.078875 11.855 0.046875 11.709 0.046875C11.353 0.046875 11.18 0.211875 11.155 0.228875C11.073 0.270875 11.005 0.337875 11.004 0.338875L4.698 6.08888L1.951 4.00488C1.832 3.91388 1.69 3.86987 1.548 3.86987C1.387 3.86987 1.226 3.92788 1.1 4.04288L0.219 4.84387C0.074 4.97587 0.001 5.15688 0.001 5.33687C0.001 5.51687 0.073 5.69688 0.218 5.82888L2.6 8.00088L0.217 10.1719C0.072 10.3039 0 10.4839 0 10.6639C0 10.8439 0.073 11.0249 0.218 11.1569L1.099 11.9579C1.226 12.0729 1.386 12.1309 1.547 12.1309C1.688 12.1309 1.83 12.0859 1.95 11.9959L4.697 9.91187L11.003 15.6619C11.003 15.6619 11.072 15.7299 11.155 15.7719C11.179 15.7889 11.353 15.9529 11.709 15.9529C11.855 15.9529 12.003 15.9209 12.141 15.8549L15.435 14.2709C15.781 14.1049 16.001 13.7539 16.001 13.3699V2.62888C16.001 2.24487 15.781 1.89488 15.435 1.72787L15.434 1.72887ZM7.217 7.99988L12.002 4.36987V11.6299L7.217 7.99988Z"</svg>" width="30" height="30"></a><br>VSCode | The Open Source AI Code Editor |

## Project Patterns

### Web UI
* The Web UI uses [Vite](https://vitejs.dev) as its build system.
* The HTML pages used by the Web UI are found in `./src_assets/common/assets/web`.
* [EJS](https://www.npmjs.com/package/vite-plugin-ejs) is used as a templating system for the pages
  (check `template_header.html` and `template_header_main.html`).
* The Style System is now powered by [Tailwind CSS](https://tailwindcss.com). (Bootstrap has been removed; a lightweight shim layer maps a few legacy classes like `btn` and `form-control` to Tailwind utilities for backward compatibility.)
* Icons are provided by [Lucide](https://lucide.dev) and [Simple Icons](https://simpleicons.org).
* The JS framework used by the more interactive pages is [Vus.js](https://vuejs.org).

#### Routing Mode (History API)

The Vue router is configured with `createWebHistory('/')` (no hash fragment). The C++ config HTTP server provides a SPA fallback (`getSpaEntry` in `src/confighttp.cpp`) that serves `index.html` for any non-API, non-static route so deep links and refreshes work. When adding a new top‑level UI path under `/`, no backend change is needed unless it conflicts with existing static prefixes (`/api`, `/assets`, `/covers`, `/images`).

#### Tailwind CSS Integration

Tailwind utilities are compiled via PostCSS. The entry stylesheet `src_assets/common/assets/web/styles/tailwind.css` includes:

```
@tailwind base;
@tailwind components;
@tailwind utilities;
```

and is imported in `main.js`. The purge/content configuration lives in `tailwind.config.js`:

```
content: ['./src_assets/common/assets/web/**/*.{html,js,ts,vue}']
```

If you place Vue/HTML/TS files outside that tree, extend the glob so their class names are not purged. Tailwind `preflight` is enabled; any remaining Bootstrap-era markup should be updated to utilities. A shim layer in `styles/tailwind.css` defines legacy class aliases (`.btn`, variants, `.form-control`) to ease incremental refactors. Prefer replacing those with first-class Tailwind utilities over time. For dynamic class generation (string concatenation) add a `safelist` in `tailwind.config.js` instead of dummy elements.

#### Building

As of the release of LuminalShine, the standard build workflow has changed greatly from Sunshine and Vibeshine, with it now using ``clang`` with ``--fms-extensions`` to build the project. This allows MSVC-specific features to be implemented into LuminalShine without fully depending on MSVC for the build process.

### Localization
LuminalShine and related NortheBridge Software Foundation projects are localized into into `en` (English, US).
This is the default language, and currently, there are no plans to localize it for other languages.

#### Extraction

##### Web UI
Sunshine uses [Vue I18n](https://vue-i18n.intlify.dev) for localizing the UI.
The following is a simple example of how to use it.

* Add the string to the `./src_assets/common/assets/web/public/assets/locale/en.json` file, in English.
  ```json
  {
   "index": {
     "welcome": "Hello, LuminalShine!"
   }
  }
  ```

  > [!NOTE]
  > The JSON keys should be sorted alphabetically. You can use [jsonabc](https://novicelab.org/jsonabc)
  > to sort the keys.

> [!WARNING]
> The below is for information only. Contributors should never include manually updated template files, or
> manually compiled language files in Pull Requests.

Strings are automatically extracted from the code to the `locale/sunshine.po` template file. The generated file is
used by CrowdIn to generate language specific template files. The file is generated using the
`.github/workflows/localize.yml` workflow and is run on any push event into the `master` branch. Jobs are only run if
any of the following paths are modified.

```yaml
- 'src/**'
```

When testing locally it may be desirable to manually extract, initialize, update, and compile strings. Python is
required for this, along with the python dependencies in the `./scripts/requirements.txt` file. Additionally,
[xgettext](https://www.gnu.org/software/gettext) must be installed.

* Extract, initialize, and update
  ```bash
  python ./scripts/_locale.py --extract --init --update
  ```

* Compile
  ```bash
  python ./scripts/_locale.py --compile
  ```

> [!IMPORTANT]
> Due to the integration with CrowdIn, it is important to not include any extracted or compiled files in
> Pull Requests. The files are automatically generated and updated by the workflow. Once the PR is merged, the
> translations can take place on [CrowdIn][crowdin-url]. Once the translations are
> complete, a PR will be made to merge the translations into Sunshine.

<div class="section_buttons">

| Previous                |                                                         Next |
|:------------------------|-------------------------------------------------------------:|
| [Building](building.md) | [Source Code](../third-party/doxyconfig/docs/source_code.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
