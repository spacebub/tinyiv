# Compiles a file in as `Embedded::<symbol>` and `Embedded::<symbol>Size`, so
# nothing has to be found on disk at runtime.
function(tiv_embed target symbol input)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/embed/${symbol}.cpp")

    add_custom_command(
            OUTPUT ${generated}
            COMMAND ${CMAKE_COMMAND}
            -DTIV_EMBED_INPUT=${input}
            -DTIV_EMBED_OUTPUT=${generated}
            -DTIV_EMBED_SYMBOL=${symbol}
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFile.cmake
            DEPENDS ${input} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFile.cmake
            COMMENT "Embedding ${symbol}"
            VERBATIM)

    target_sources(${target} PRIVATE ${generated})
endfunction()
