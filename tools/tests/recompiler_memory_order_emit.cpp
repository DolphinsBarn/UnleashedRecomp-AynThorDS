// Synthetic PPC instructions only; no game executable or generated game code.
#include "recompiler.h"
#include <iostream>

struct TestInstruction
{
    const char* name;
    uint32_t word;
    int id;
};

static constexpr uint32_t XForm(unsigned rt, unsigned ra, unsigned rb, unsigned xo, bool rc = false)
{
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | unsigned(rc);
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;

    const TestInstruction instructions[] = {
        { "test_sync",   0x7C0004AC, PPC_INST_SYNC },
        { "test_lwsync", 0x7C2004AC, PPC_INST_LWSYNC },
        { "test_eieio",  0x7C0006AC, PPC_INST_EIEIO },
        { "test_isync",  0x4C00012C, PPC_INST_ISYNC },
        { "test_lwarx_zero", XForm(3, 0, 4, 20), PPC_INST_LWARX },
        { "test_lwarx", XForm(3, 5, 4, 20), PPC_INST_LWARX },
        { "test_ldarx", XForm(3, 5, 4, 84), PPC_INST_LDARX },
        { "test_stwcx", XForm(6, 5, 4, 150, true), PPC_INST_STWCX },
        { "test_stdcx", XForm(6, 5, 4, 214, true), PPC_INST_STDCX },
    };

    std::ofstream output(argv[1]);
    output << "#define PPC_CONFIG_H_INCLUDED\n#include <ppc_context.h>\n";
    for (const auto& instruction : instructions)
    {
        // Include a second instruction because the emitter looks ahead when
        // identifying MMIO stores. Keep the byte order expected by its decoder.
        uint32_t words[] = { ByteSwap(instruction.word), ByteSwap(0x60000000u) };
        ppc_insn decoded{};
        if (ppc::Disassemble(words, sizeof(words), 0x1000, decoded) != 4 ||
            decoded.opcode->id != instruction.id)
        {
            std::cerr << "Failed to decode " << instruction.name << '\n';
            return 1;
        }

        Recompiler recompiler;
        RecompilerLocalVariables locals{};
        CSRState csr = CSRState::Unknown;
        auto switchTable = recompiler.config.switchTables.end();
        if (!recompiler.Recompile(Function(0x1000, 4), 0x1000, decoded,
                                  words, switchTable, locals, csr))
        {
            std::cerr << "Unsupported instruction: " << instruction.name << '\n';
            return 1;
        }

        output << "PPC_FUNC_IMPL(" << instruction.name << ") {\n"
               << recompiler.out << "}\n";
    }
    return output.good() ? 0 : 1;
}
