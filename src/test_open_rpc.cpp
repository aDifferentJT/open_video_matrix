#include "open_rpc.hpp"

#include <iostream>

void add(int x, int y) { std::cout << x + y << '\n'; }

struct curried_adder {
  int x;

  void add(int y) { std::cout << x + y << '\n'; }
};

struct wrapper {
  curried_adder adder;
};

struct counter {
  int x;

  void inc() { x += 1; }
  void show() { std::cout << x << '\n'; }

  static constexpr auto orpc_members = std::tuple{
      std::pair{"inc", &counter::inc}, std::pair{"show", &counter::show}};
};

static_assert(!ORPC::has_orpc_members<curried_adder>);
static_assert(!ORPC::has_orpc_members<wrapper>);
static_assert(ORPC::has_orpc_members<counter>);

int main() {
  auto router = ORPC::router{};
  router.add("add1", add);
  router.add("add2", &add);
  router.add("add3", *&add);
  router.add("add4", [](int x, int y) { std::cout << x + y << '\n'; });
  router.process({"", {1, 2}});

  router.process({"/add1", {1, 2}});
  router.process({"/add2", {1, 2}});
  router.process({"/add3", {1, 2}});
  router.process({"/add4", {1, 2}});

  auto curried_adder_obj = curried_adder{3};
  auto &curried_adder_rpc = router.add("curried_adder", curried_adder_obj);
  curried_adder_rpc.add("add", &curried_adder::add);
  curried_adder_rpc.add("add_standalone", add);
  router.process({"/curried_adder/add", {4}});
  router.process({"/curried_adder/add_standalone", {1, 2}});

  auto wrapper_obj = wrapper{5};
  auto &wrapper_rpc = router.add("wrapper", wrapper_obj);
  auto &curried_adder2_rpc = wrapper_rpc.add("adder", &wrapper::adder);
  curried_adder2_rpc.add("add", &curried_adder::add);
  router.process({"/wrapper/adder/add", {6}});
  router.process({"/wrapper/adder", {6}});

  router.add("counter1", counter{0});
  router.process({"/counter1/inc", {}});
  router.process({"/counter1/show", {}});

  auto counter2 = counter{0};
  router.add("counter2", counter2);
  router.process({"/counter2/inc", {}});
  router.process({"/counter2/show", {}});
  std::cout << counter2.x << '\n';
}
