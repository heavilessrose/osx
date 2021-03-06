// RUN: clang-cc -fsyntax-only -verify %s

struct foo { int a; };

int main() {
  int a;
  float b;
  double d;
  struct foo s;

  static int ary[__builtin_classify_type(a)];
  static int ary2[(__builtin_classify_type)(a)]; // expected-error{{variable length array declaration can not have 'static' storage duration}}
  static int ary3[(*__builtin_classify_type)(a)]; // expected-error{{variable length array declaration can not have 'static' storage duration}}

  int result;

  result =  __builtin_classify_type(a);
  result =  __builtin_classify_type(b);
  result =  __builtin_classify_type(d);
  result =  __builtin_classify_type(s);
}
