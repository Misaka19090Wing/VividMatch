# Third-party notices

VividMatch is distributed under the MIT licence; see `LICENSE`. It borrows from
the project below, whose licence is reproduced here as that licence requires.

## XMuli/myapp-template

The GUI's window/page structure and CMake layout come from
[XMuli/myapp-template](https://github.com/XMuli/myapp-template), which the
project's own documentation states is MIT licensed. Its MIT text follows.

```
MIT License

Copyright (c) XMuli

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

Note: the copyright line above reproduces the standard MIT form with the holder
named as `XMuli`; confirm it against the template's own `LICENSE` file, which was
not reachable when this note was written. If the template turns out to be under a
different licence, replace this section with that licence's text.

## Qt and OpenCV

The portable build in `dist/` bundles Qt 6 (LGPLv3 / commercial) and OpenCV 5
(Apache-2.0) as separate dynamic libraries. They are used unmodified and are not
covered by this project's MIT licence; their own terms apply to them. The Qt
sources are available from <https://www.qt.io/> and OpenCV from
<https://opencv.org/>.
