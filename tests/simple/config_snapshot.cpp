// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>
#include <easy/snapshot.h>

#include <cstdio>

struct NestedConfig {
  int id;
  bool enabled;
  int values[4];
};

struct PrimaryConfig {
  int base;
  bool enabled;
  int weight;
  int data[4];
  NestedConfig nested;
};

int eval_snapshot(int x, PrimaryConfig const &cfg) {
  int result = x;
  if (cfg.enabled) {
    result += cfg.base;
    result += cfg.data[0];
  } else {
    result -= cfg.weight;
  }

  if (cfg.nested.enabled) {
    result += cfg.nested.values[2];
  } else {
    result += cfg.nested.id;
  }

  return result;
}

int main() {
  PrimaryConfig cfg = {};
  cfg.base = 100;
  cfg.enabled = true;
  cfg.weight = 7;
  cfg.data[0] = 11;
  cfg.nested.id = 3;
  cfg.nested.enabled = true;
  cfg.nested.values[0] = 5;
  cfg.nested.values[1] = 6;
  cfg.nested.values[2] = 33;
  cfg.nested.values[3] = 8;

  auto fn = easy::jit(eval_snapshot, 5, easy::snapshot(cfg));

  // CHECK: snapshot.result=149
  std::printf("snapshot.result=%d\n", fn());
  return 0;
}
