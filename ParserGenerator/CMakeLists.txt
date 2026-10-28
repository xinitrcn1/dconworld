project(ParserGenerator LANGUAGES CXX)

add_executable(ParserGenerator parser_generator.cpp)
target_compile_features(ParserGenerator PRIVATE cxx_std_20)
