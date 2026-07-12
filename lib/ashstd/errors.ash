// ashstd.errors: the shared error sums the rest of the standard library
// leans on. CommonError covers the three failures a small API keeps meeting:
// a lookup that found nothing, an argument that made no sense, and a source
// that ran dry. It is an ordinary sum, so it rides the E slot of any Result
// and crosses the ABI as a tagged value like every other sum. Modules with a
// failure vocabulary of their own, the way ashstd.math has AshMathError,
// declare it beside their contract instead.

CommonError is either NotFound or Invalid or Exhausted
