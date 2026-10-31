project(ParserGenerator LANGUAGES CXX)

add_executable(ParserGenerator parser_generator.cpp)
target_compile_features(ParserGenerator PRIVATE cxx_std_20)
target_compile_options(ParserGenerator PRIVATE -fno-rtti -fno-exceptions)
