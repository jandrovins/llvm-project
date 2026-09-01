// MISSING-DATA: error: failed to open replay data file
// GENERATE: serialized input =
// GENERATE: result[0] = 81
// REPLAY: replay result[0] = 81
// CAPACITY-CONFLICT: error: replay options conflict with the InputGen data file
// OUTPUT-ERROR: error: failed to open generated data file

struct Inputs {
  int *Values;
  int Count;
};

__attribute__((noinline)) int reduce(int *Values, int Count) {
  int Sum = 0;
  for (int I = 0; I < Count; ++I)
    Sum += Values[I];
  return Sum;
}

int vvv_foo(struct Inputs *Input) {
  return reduce(Input->Values, Input->Count);
}
