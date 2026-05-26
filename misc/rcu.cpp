#include <atomic>
#include <memory>
#include <string>
#include <concepts>

template <typename T>
class rcu {
	public:

	using value_type = T;

	rcu(const rcu&) = delete;
	rcu(rcu&&) = delete;
	auto operator=(const rcu&) -> rcu& = delete;
	auto operator=(rcu&&) -> rcu& = delete;
	rcu()
		: _value{std::make_shared<const T>()} {}
	explicit rcu(std::shared_ptr<const T> value)
		: _value{std::move(value)} {}
	explicit rcu(T&& value)
		: _value{std::make_shared<const T>(std::move(value))} {}
	template <typename... Args>
	requires std::constructible_from<T, Args...>
	explicit rcu(std::in_place_t /*unused*/, Args&&... args)
		: _value{std::make_shared<const T>(std::forward<Args>(args)...)} {}

	auto read() const -> std::shared_ptr<const T> {
		return _value.load();
	}
	template <typename F>
	requires std::invocable<F&, T&>
	void mutate(F&& func) {
		std::shared_ptr<const T> current = _value.load();
		while (true) {
			// copy
			std::shared_ptr<T> copy = std::make_shared<T>(*current);
			// modify
			func(*copy);
			// store
			std::shared_ptr<const T> new_value{std::move(copy)};
			if (_value.compare_exchange_weak(current, new_value)) {
				break;
			}
		}
	}
	private:

	std::atomic<std::shared_ptr<const T>> _value;
};

template class rcu<std::string>;
