# Copies IN to OUT dropping // comments (the DataContainer format has none)
file(READ "${IN}" contents)
string(REGEX REPLACE "//[^\n]*" "" contents "${contents}")
file(WRITE "${OUT}" "${contents}")
