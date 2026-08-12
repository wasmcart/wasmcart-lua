# API status tooling

Two halves, and both are needed:

- `enumerate.lua` — run as a cart. Walks the `love` table by REFLECTION and
  prints every function that really exists. Grep would miss functions
  defined indirectly and, worse, could report one the engine does not have.
- `gen-status.mjs` — turns that dump into `API_STATUS.md`.

`love-api.txt` is the denominator: the LOVE function list from libretro's
[lutro-status](https://github.com/libretro/lutro-status), used so our
percentage and Lutro's mean the same thing. Picking our own denominator
would be marking our own homework.

## The thing this tooling CANNOT tell you

Presence is not conformance. A function can be exported and still return
nonsense, and a coverage percentage built only from names is exactly the
kind of number that gets quoted and then turns out to be hollow.

`test/apiconform/` is the other half. It asserts on VALUES — round-trips,
known-answer maths, conserved areas, distributions — and it has been
verified against a deliberately broken build (the 2.2 gamma approximation
in place of the real sRGB curve), which it caught with 3 failures. A test
that cannot fail proves nothing.
