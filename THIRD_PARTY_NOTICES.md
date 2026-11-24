# Third-party notices

## DaisySeedProjects A2-Lite runtime

The optional `make setup-a2` comparison/fallback setup downloads `nam_a2_runtime.h` from
[bkshepherd/DaisySeedProjects](https://github.com/bkshepherd/DaisySeedProjects)
at commit `ccae0f2305881d9f706557404ba22c0675c97bd7`.

DaisySeedProjects is distributed under the MIT License. Its source checkout,
including its license text and attribution, is retained under
`external/DaisySeedProjects` and is excluded from this repository's commits.

The local supplemental linker section follows the memory-placement interface
required by that runtime. The Hothouse integration and model validation tooling
in this repository remain covered by this project's GPL-3.0 license.

## TONE3000 embedded A2-Lite engine (production default)

`make setup-tone3000` fetches [tone-3000/embedded](https://github.com/tone-3000/embedded)
at `b84b60c652cb386ea341be50b8c665ae8a489cff` (v2.0.0). The default
`NAM_ENGINE=tone3000` build uses its Cortex-M7 A2-Lite engine. It is distributed
under the MIT License, Copyright (c) 2026 TONE3000. The complete license and
copyright notice are retained in `external/tone3000-embedded/LICENSE`; retain
them when redistributing that dependency or its compiled code.

The golden test uses the dependency's sample capture, input and NAMCore
reference output only on the host; none is embedded in production firmware.
The supplemental ITCM section follows the engine's documented memory layout,
adapted to our existing BOOT_SRAM image rather than changing to BOOT_QSPI.

The engine's license text follows:

```text
MIT License

Copyright (c) 2026 TONE3000

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
