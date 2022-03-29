find . -name "*.c" | while read -r line; do clang-format -i $line; done
find . -name "*.h" | while read -r line; do clang-format -i $line; done
