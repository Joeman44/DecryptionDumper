#include "Disassembler.h"
#include "Debugger.h"
#include "PatternScanner.h"
#include <regex>
#include "ContextRestorer.h"

Disassembler::Disassembler(Debugger* dbg) : debugger(dbg)
{
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);
	ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
}

ZydisDecodedInstruction Disassembler::Decode(uintptr_t rip) const
{
	ZydisDecodedInstruction instruction;
	BYTE bRead[20];
	debugger->read_array(rip, bRead, 20);

	if (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(
		&decoder, bRead, 20,
		&instruction))) {
		return instruction;
	}
	memset(&instruction, 0, sizeof(ZydisDecodedInstruction));
	return instruction;
}

void Disassembler::SkipOverUntilInstruction(ZydisMnemonic mnemonic)
{
	ZydisDecodedInstruction instruction = Decode(current_rip);
	while (instruction.mnemonic != mnemonic)
	{
		current_rip += instruction.length;
		instruction = Decode(current_rip);
	}
	current_rip += instruction.length;
	debugger->SetRIP(current_rip);
}

void Disassembler::SkipUntilInstruction(ZydisMnemonic mnemonic)
{
	ZydisDecodedInstruction instruction = Decode(current_rip);
	while (instruction.mnemonic != mnemonic)
	{
		current_rip += instruction.length;
		instruction = Decode(current_rip);
	}
	debugger->SetRIP(current_rip);
}

void Disassembler::RunUntilInstruction(ZydisMnemonic mnemonic)
{
	ZydisDecodedInstruction instruction = Decode(current_rip);
	while (instruction.mnemonic != mnemonic)
	{

		uintptr_t rip = debugger->SingleStep();
		if (debugger->exception_hit) {
			current_rip += instruction.length; //if exception is caused the ptr is not advanced.
			debugger->SetRIP(current_rip);
			debugger->exception_hit = false;
		}
		current_rip = rip;
		instruction = Decode(current_rip);
	}
}

void Disassembler::GoToAddress(uintptr_t address)
{
	current_rip = address;
	debugger->SetRIP(current_rip);
}

ZydisRegister Disassembler::To64BitRegister(ZydisRegister reg) const
{
	if (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg) == 32)
	{
		ZyanI16 regID = ZydisRegisterGetId(reg);
		reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, regID);
	}
	return reg;
}

std::string Disassembler::Get64BitRegisterString(ZydisRegister reg) const
{
	return ZydisRegisterGetString(To64BitRegister(reg));
}

void Disassembler::GetModifiedRegisters(ZydisDecodedInstruction instruction, ZydisRegister reg[8]) const
{
	for (uint32_t i = 0; i < instruction.operand_count; i++)
	{
		if (instruction.operands[i].visibility == ZydisOperandVisibility::ZYDIS_OPERAND_VISIBILITY_EXPLICIT
			|| To64BitRegister(instruction.operands[i].reg.value) == ZydisRegister::ZYDIS_REGISTER_RAX //RAX is implicit? idk lol
			|| instruction.mnemonic == ZydisMnemonic::ZYDIS_MNEMONIC_AND
			|| instruction.mnemonic == ZydisMnemonic::ZYDIS_MNEMONIC_MUL) { //ZydisMnemonic::ZYDIS_MNEMONIC_AND or ZYDIS_MNEMONIC_MUL-> operand 0 is implicit ... for whatever reason..
			if (instruction.operands[i].type == ZydisOperandType::ZYDIS_OPERAND_TYPE_REGISTER) {
				if (instruction.operands[i].actions & ZydisOperandAction::ZYDIS_OPERAND_ACTION_WRITE)
					reg[i] = To64BitRegister(instruction.operands[i].reg.value);
			}
			else if (instruction.operands[i].type == ZydisOperandType::ZYDIS_OPERAND_TYPE_MEMORY) {
				if (instruction.operands[i].actions & ZydisOperandAction::ZYDIS_OPERAND_ACTION_WRITE)
					reg[i] = To64BitRegister(instruction.operands[i].mem.base);
			}
		}
	}
}

void Disassembler::GetAccessedRegisters(ZydisDecodedInstruction instruction, ZydisRegister reg[8]) const
{
	for (uint32_t i = 0; i < instruction.operand_count; i++)
	{
		if (instruction.operands[i].visibility == ZydisOperandVisibility::ZYDIS_OPERAND_VISIBILITY_EXPLICIT
			|| To64BitRegister(instruction.operands[i].reg.value) == ZydisRegister::ZYDIS_REGISTER_RAX //RAX is implicit? idk lol
			|| instruction.mnemonic == ZydisMnemonic::ZYDIS_MNEMONIC_AND
			|| instruction.mnemonic == ZydisMnemonic::ZYDIS_MNEMONIC_MUL) { //ZydisMnemonic::ZYDIS_MNEMONIC_AND or ZYDIS_MNEMONIC_MUL-> operand 0 is implicit ... for whatever reason..
			if (instruction.operands[i].type == ZydisOperandType::ZYDIS_OPERAND_TYPE_REGISTER) {
				if (instruction.operands[i].actions & ZydisOperandAction::ZYDIS_OPERAND_ACTION_READ)
					reg[i] = To64BitRegister(instruction.operands[i].reg.value);
			}
			else if (instruction.operands[i].type == ZydisOperandType::ZYDIS_OPERAND_TYPE_MEMORY) {
				if (instruction.operands[i].actions & ZydisOperandAction::ZYDIS_OPERAND_ACTION_READ || (instruction.mnemonic == ZydisMnemonic::ZYDIS_MNEMONIC_LEA && i > 0)) {
					if (instruction.operands[i].mem.base != ZydisRegister::ZYDIS_REGISTER_RIP && instruction.operands[i].mem.base != ZydisRegister::ZYDIS_REGISTER_RBP && instruction.operands[i].mem.base != ZydisRegister::ZYDIS_REGISTER_RSP)
						reg[i] = To64BitRegister(instruction.operands[i].mem.base);
					if (instruction.operands[i].mem.index)
						reg[i + 4] = To64BitRegister(instruction.operands[i].mem.index);
				}
			}
		}
	}
}

std::string Disassembler::AsmToCPP(ZydisDecodedInstruction instruction, uintptr_t rip, const char* stack_trace_name) const
{
	std::stringstream ss;
	ZydisRegister r1 = instruction.operands[0].reg.value;
	ZydisRegister r2 = instruction.operands[1].reg.value;
	ZydisRegister r3 = instruction.operands[2].reg.value;
	ZydisRegister r4 = instruction.operands[3].reg.value;
	switch (instruction.mnemonic)
	{
	case ZYDIS_MNEMONIC_LEA:
		//LEA	r16/32,	m
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP) {
			ss << Get64BitRegisterString(r1) << " = " << "baseModuleAddr";
			if ((rip + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address != 0)
				ss << " + 0x" << std::hex << std::uppercase << (rip + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address;
		}
		// LEA   RAX,[RAX + RCX * 0x2]
		else if (instruction.operands[1].mem.index != 0 && instruction.operands[1].mem.scale != 0)
		{
			if (instruction.operands[1].mem.base != ZydisRegister::ZYDIS_REGISTER_NONE)
				ss << Get64BitRegisterString(r1) << " = " << Get64BitRegisterString(instruction.operands[1].mem.base) << " + " << Get64BitRegisterString(instruction.operands[1].mem.index) << " * " << (int)instruction.operands[1].mem.scale;
			else
				ss << Get64BitRegisterString(r1) << " = " << Get64BitRegisterString(instruction.operands[1].mem.index) << " * " << (int)instruction.operands[1].mem.scale << " + 0x" << instruction.operands[1].mem.disp.value;
		}
		else
		{
			ss << Get64BitRegisterString(r1) << " = " << Get64BitRegisterString(instruction.operands[1].mem.base) << " + 0x" << std::hex << instruction.operands[1].mem.disp.value;
		}
		break;
	case ZYDIS_MNEMONIC_MOV:
		if (instruction.operands[0].type == ZydisOperandType::ZYDIS_OPERAND_TYPE_REGISTER)
		{
			switch (instruction.operands[1].type)
			{
			case ZydisOperandType::ZYDIS_OPERAND_TYPE_REGISTER:
				ss << Get64BitRegisterString(r1) << " = " << Get64BitRegisterString(r2);
				break;
			case ZydisOperandType::ZYDIS_OPERAND_TYPE_MEMORY:
				if (instruction.operands[1].mem.segment == ZYDIS_REGISTER_GS)
				{
					ss << Get64BitRegisterString(r1) << " = " << "Peb";
				}
				else if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
				{
					ss << Get64BitRegisterString(r1) << " = " << "*(uintptr_t*)(baseModuleAddr + 0x" << std::hex << std::uppercase << (rip + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address << ")";
				}
				else if (stack_trace_name) {
					ss << Get64BitRegisterString(r1) << " = " << stack_trace_name;
				}
				else if (instruction.operands[1].mem.disp.has_displacement)
				{
					ss << Get64BitRegisterString(r1) << " = *(uintptr_t*)(" << Get64BitRegisterString(instruction.operands[1].mem.base) << " + 0x" << std::hex << instruction.operands[1].mem.disp.value << ")";
				}
				else
				{
					ss << Get64BitRegisterString(r1) << " = *(uintptr_t*)(" << Get64BitRegisterString(instruction.operands[1].mem.base) << ")";
				}
				break;
			case ZydisOperandType::ZYDIS_OPERAND_TYPE_IMMEDIATE:
				if (instruction.operands[1].imm.is_signed)
					ss << Get64BitRegisterString(r1) << " = " << "0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.s;
				else
					ss << Get64BitRegisterString(r1) << " = " << "0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.u;
				break;
			default:
				break;
			}
		}
		//Register to Register

		break;

	case ZYDIS_MNEMONIC_MOVZX:
	case ZYDIS_MNEMONIC_MOVSX:
		// MOVSX    R15D,word ptr [RCX + R11*0x1 + 0x4dfb360]
		if (instruction.operand_count == 2 && instruction.operands[1].mem.base != 0 && instruction.operands[1].mem.index != 0 && instruction.operands[1].mem.disp.value != 0)
		{
			ss << Get64BitRegisterString(r1) << " = *(uint16_t*)(" << std::uppercase << Get64BitRegisterString(instruction.operands[1].mem.base) << " + " << Get64BitRegisterString(instruction.operands[1].mem.index) << " * "
				<< (int)instruction.operands[1].mem.scale << " + 0x" << std::hex << instruction.operands[1].mem.disp.value << ")";
		}
		else if (stack_trace_name) {
			ss << Get64BitRegisterString(r1) << " = " << stack_trace_name;
		}
		else
			ss << GetInstructionText(instruction);

		break;
	case ZYDIS_MNEMONIC_ROR:
		ss << Get64BitRegisterString(r1) << " = _rotr64(" << Get64BitRegisterString(r1) << ", 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.u << ")";
		break;
	case ZYDIS_MNEMONIC_ROL:
		ss << Get64BitRegisterString(r1) << " = _rotl64(" << Get64BitRegisterString(r1) << ", 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.u << ")";
		break;
	case ZYDIS_MNEMONIC_SHR:
		ss << Get64BitRegisterString(r1) << " >>= 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.s;
		break;
	case ZYDIS_MNEMONIC_SHL:
		ss << Get64BitRegisterString(r1) << " <<= 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.s;
		break;
	case ZYDIS_MNEMONIC_SUB:
		//Reg to Reg
		if (instruction.operand_count == 3 && r2 != 0)
		{
			ss << Get64BitRegisterString(r1) << " -= " << Get64BitRegisterString(r2);
		}
		else if (instruction.operand_count >= 2 && instruction.operands[1].imm.value.s != 0)
		{
			if (instruction.operands[1].imm.is_signed)
				ss << Get64BitRegisterString(r1) << " -= 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.s;
			else
				ss << Get64BitRegisterString(r1) << " -= 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.u;
		}
		else if (stack_trace_name) {
			ss << Get64BitRegisterString(r1) << " -= " << stack_trace_name;
		}
		else
			ss << GetInstructionText(instruction);
		break;
	case ZYDIS_MNEMONIC_ADD:
		//Reg to Reg
		if (instruction.operand_count == 3 && r2 != 0)
		{
			ss << Get64BitRegisterString(r1) << " += " << std::uppercase << Get64BitRegisterString(r2);
		}
		//ADD   RCX, 0x236d1de3
		else if (instruction.operand_count >= 2 && instruction.operands[1].imm.value.s != 0)
		{
			if (instruction.operands[1].imm.is_signed)
				ss << Get64BitRegisterString(r1) << " += 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.s;
			else
				ss << Get64BitRegisterString(r1) << " += 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.u;
		}
		else if (stack_trace_name) {
			ss << Get64BitRegisterString(r1) << " += " << stack_trace_name;
		}
		else
			ss << GetInstructionText(instruction);
		break;
	case ZYDIS_MNEMONIC_AND:
		//Reg to Value
		if (instruction.operands[1].imm.value.s != 0 && instruction.operands[0].reg.value != 0)
		{
			if (instruction.operands[1].imm.value.s != 0xffffffffc0000000) {
				if (instruction.operands[1].imm.is_signed) {
					ss << Get64BitRegisterString(r1) << " " << " &= 0x" << std::hex << instruction.operands[1].imm.value.s;
				}
				else {
					ss << Get64BitRegisterString(r1) << " " << " &= 0x" << std::hex << instruction.operands[1].imm.value.u;
				}
			}
			else
				ss << Get64BitRegisterString(r1) << " = 0";
		}
		//Reg to Reg
		else if (instruction.operands[0].reg.value != 0 && r2 != 0)
		{
			ss << Get64BitRegisterString(r1) << " &= " << Get64BitRegisterString(r2);
		}
		else
		{
			ss << GetInstructionText(instruction);
		}

		break;
	case ZYDIS_MNEMONIC_XOR:
		if (stack_trace_name) {
			ss << Get64BitRegisterString(r1) << " ^= " << stack_trace_name;
		}
		else if (instruction.operands[1].mem.disp.value != 0)
		{
			ss << Get64BitRegisterString(r1) << " ^= " << "*(uintptr_t*)(baseModuleAddr + 0x" << std::hex << std::uppercase << (rip + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address << ")";
		}
		else
		{
			ss << Get64BitRegisterString(r1) << " ^= " << Get64BitRegisterString(r2);
		}

		break;
	case ZYDIS_MNEMONIC_BSWAP:
		ss << Get64BitRegisterString(r1) << " = _byteswap_uint64(" << Get64BitRegisterString(r1) << ")";
		break;
	case ZYDIS_MNEMONIC_NOT:
		ss << Get64BitRegisterString(r1) << " = ~" << Get64BitRegisterString(r1);
		break;
	case ZYDIS_MNEMONIC_MUL:
		if (instruction.operand_count == 4)
		{
			ss << Get64BitRegisterString(r2) << std::uppercase << " = _umul128(" << Get64BitRegisterString(r2) << ", " << Get64BitRegisterString(r1) << ", (uintptr_t*)&" << Get64BitRegisterString(r3) << ")";
		}
		else
			ss << GetInstructionText(instruction);
		break;
	case ZYDIS_MNEMONIC_IMUL:
		//Reg to Reg
		if ((instruction.operand_count == 2 || instruction.operand_count == 3) && r2 != 0)
		{
			ss << Get64BitRegisterString(r1) << " *= " << Get64BitRegisterString(r2);
		}
		//Value
		else if (instruction.operand_count == 2 && instruction.operands[1].imm.value.s != 0)
		{
			if (instruction.operands[1].imm.is_signed)
				ss << Get64BitRegisterString(r1) << " *= 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.s;
			else
				ss << Get64BitRegisterString(r1) << " *= 0x" << std::hex << std::uppercase << instruction.operands[1].imm.value.u;
		}
		//IMUL  RAX,qword ptr [RCX + 0xb]
		else if (instruction.operands[1].mem.base != 0 && instruction.operands[1].mem.disp.has_displacement)
		{
			if (instruction.operands[1].mem.base != ZYDIS_REGISTER_RSP && instruction.operands[1].mem.base != ZYDIS_REGISTER_RBP)
				ss << Get64BitRegisterString(r1) << " *= " << "*(uintptr_t*)(" << Get64BitRegisterString(instruction.operands[1].mem.base) << " + 0x" << std::hex << instruction.operands[1].mem.disp.value << ")";
		}
		//IMUL  RAX,RAX,0x25a3
		else if (instruction.operand_count == 4 && instruction.operands[0].reg.value != 0 && r2 != 0 && instruction.operands[2].imm.value.s != 0)
		{
			ss << Get64BitRegisterString(r1) << " = " << Get64BitRegisterString(r2) << " * 0x" << std::hex << std::uppercase << instruction.operands[2].imm.value.s;
		}
		else if (stack_trace_name) {
			ss << Get64BitRegisterString(r1) << " *= " << stack_trace_name;
		}
		else
		{
			ss << GetInstructionText(instruction);
		}
		break;
	case ZYDIS_MNEMONIC_CALL:
	case ZYDIS_MNEMONIC_JNZ:
	case ZYDIS_MNEMONIC_JMP:
	case ZYDIS_MNEMONIC_NOP:
	case ZYDIS_MNEMONIC_JNBE:
	case ZYDIS_MNEMONIC_CMP:
	case ZYDIS_MNEMONIC_TEST:
	case ZYDIS_MNEMONIC_JZ:
		break;
	default:
		//ss << "//?? " << std::hex << rip - debugger->base_address;
		break;
	}
	return ss.str();
}

std::string Disassembler::GetInstructionText(ZydisDecodedInstruction& instruction) const {
	char DisassembledString[256];
	ZydisFormatterFormatInstruction(&formatter, &instruction, DisassembledString, sizeof(DisassembledString), 0);
	return std::string(DisassembledString);
}

bool Disassembler::Print_PEB()
{
	ZydisDecodedInstruction instruction;
	bool checkNotPeb = false;

	int i = 0;


	while (i < 15)
	{
		instruction = Decode(current_rip);
		current_rip += instruction.length;
		if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.operands[1].mem.segment == ZYDIS_REGISTER_GS)
		{
			char DisassembledString[256];
			ZydisFormatterFormatInstruction(&formatter, &instruction, DisassembledString, sizeof(DisassembledString), 0);

			ZydisDecodedInstruction next_instruction = Decode(current_rip);
			if (next_instruction.mnemonic == ZYDIS_MNEMONIC_NOT)
				printf("\t%s; \t\t//%s\n", ((std::string)ZydisRegisterGetString(instruction.operands[0].reg.value) + "= ~Peb").c_str(), DisassembledString);
			else
				printf("\t%s; \t\t//%s\n", ((std::string)ZydisRegisterGetString(instruction.operands[0].reg.value) + " = Peb").c_str(), DisassembledString);
			ignore_trace.push_back(instruction.operands[0].reg.value);
			return true;;
		}
		i++;
	}
	return false;
}

void Disassembler::AddRequiredInstruction(std::vector<InstructionTrace>& instruction_trace, std::vector<InstructionTrace>::iterator trace) const
{
#ifdef DEBUG
	char DisassembledString[256];
	ZydisFormatterFormatInstruction(&formatter, &(trace->instruction), DisassembledString, sizeof(DisassembledString), 0);
	printf("needed line %d: %s\n", trace - instruction_trace.begin(), DisassembledString);
#endif
	ZydisRegister accessed[8] = { ZydisRegister::ZYDIS_REGISTER_NONE };
	GetAccessedRegisters(trace->instruction, accessed);
	for (size_t j = 0; j < 8; j++)
	{
		if (accessed[j] != ZydisRegister::ZYDIS_REGISTER_NONE && trace->instruction.operands[1].imm.value.s != 0xffffffffc0000000) {
			try
			{
				uint32_t trace_index = trace->last_modified.at(accessed[j]);
				if (!instruction_trace[trace_index].used) {
					instruction_trace[trace_index].used = true;
					AddRequiredInstruction(instruction_trace, (instruction_trace.begin() + trace_index));
				}
			}
			catch (const std::exception&)
			{
				if (std::find(ignore_trace.begin(), ignore_trace.end(), accessed[j]) == ignore_trace.end()) {
					uintptr_t offset = (To64BitRegister(accessed[j]) - ZydisRegister::ZYDIS_REGISTER_RAX);
					if (*(&trace->context.Rax + offset) == debugger->base_address)
						printf("\t%s = moduleBaseAddr;", Get64BitRegisterString(accessed[j]).c_str());
					else
						printf("\t%s = %p\033[1;31m//failed to trace. base: %p It's possibly wrong\033[0m\n", Get64BitRegisterString(accessed[j]).c_str(), *(&trace->context.Rax + offset), debugger->base_address);
				}
			}
		}
	}
}

void Disassembler::Print_Decryption(std::vector<InstructionTrace>& instruction_trace, ZydisRegister enc_reg, const char* print_indexing)
{
	for (size_t j = 0; j < instruction_trace.size(); j++)
	{
		if (enc_reg == ZydisRegister::ZYDIS_REGISTER_MAX_VALUE || instruction_trace[j].used) {
			std::string DisassembledString = GetInstructionText(instruction_trace[j].instruction);

			if (instruction_trace[j].instruction.operands[1].mem.base == ZydisRegister::ZYDIS_REGISTER_RSP && instruction_trace[j].instruction.mnemonic != ZydisMnemonic::ZYDIS_MNEMONIC_PUSHFQ) {
				try {
					auto stack_trace = instruction_trace[instruction_trace[j].rsp_stack_map.at(instruction_trace[j].instruction.operands[1].mem.disp.value)];
					auto stack_instruction = stack_trace.instruction;

					char tmp_var[100];
					sprintf_s(tmp_var, 100, "RSP_0x%llX", instruction_trace[j].instruction.operands[1].mem.disp.value);
					printf("%suintptr_t %s;\n", print_indexing, tmp_var);

					std::string trace_code = AsmToCPP(stack_instruction, stack_trace.rip);
					std::string TraceDisassembledString = GetInstructionText(stack_instruction);
					trace_code = std::regex_replace(trace_code, std::regex(Get64BitRegisterString(stack_instruction.operands[0].reg.value)), tmp_var);
					if (trace_code.size() > 1)
						printf("\033[1;34m\t\t%s; \t\t//%s : %RSP+0x%llX\n\033[0m", trace_code.c_str(), TraceDisassembledString.c_str(), instruction_trace[j].instruction.operands[1].mem.disp.value);
					instruction_trace[j].instruction.operands[1] = stack_instruction.operands[0];

					std::string cpp_code = AsmToCPP(instruction_trace[j].instruction, instruction_trace[j].rip).c_str();
					cpp_code = cpp_code.replace(cpp_code.find("=") + 2, cpp_code.size(), tmp_var);
					if (cpp_code.size() > 1)
						printf("%s%s; \t\t//%s\n", print_indexing, cpp_code.c_str(), DisassembledString.c_str());
				}
				catch (const std::exception&) { // didn't find stack trace. use base;
					printf("\033[1;31m%s%s; \t\t//%s -- didn't find trace -> use base\033[0m\n", print_indexing, AsmToCPP(instruction_trace[j].instruction, instruction_trace[j].rip, "baseModuleAddr").c_str(), DisassembledString.c_str());
					continue;
				}
			}
			else if (instruction_trace[j].instruction.operands[1].mem.base == ZydisRegister::ZYDIS_REGISTER_RBP && instruction_trace[j].instruction.mnemonic != ZydisMnemonic::ZYDIS_MNEMONIC_PUSHFQ) {
				try {
					auto stack_trace = instruction_trace[instruction_trace[j].rbp_stack_map.at(instruction_trace[j].instruction.operands[1].mem.disp.value)];
					auto stack_instruction = stack_trace.instruction;

					char tmp_var[100];
					sprintf_s(tmp_var, 100, "RSP_0x%llX", instruction_trace[j].instruction.operands[1].mem.disp.value);
					printf("%suintptr_t %s;\n", print_indexing, tmp_var);

					std::string trace_code = AsmToCPP(stack_instruction, stack_trace.rip);
					std::string TraceDisassembledString = GetInstructionText(stack_instruction);
					trace_code = std::regex_replace(trace_code, std::regex(Get64BitRegisterString(stack_instruction.operands[0].reg.value)), tmp_var);
					if (trace_code.size() > 1)
						printf("\033[1;34m\t\t%s; \t\t//%s : %RBP+0x%llX\n\033[0m", trace_code.c_str(), TraceDisassembledString.c_str(), instruction_trace[j].instruction.operands[1].mem.disp.value);
					instruction_trace[j].instruction.operands[1] = stack_instruction.operands[0];

					std::string cpp_code = AsmToCPP(instruction_trace[j].instruction, instruction_trace[j].rip).c_str();
					cpp_code = cpp_code.replace(cpp_code.find("=") + 2, cpp_code.size(), tmp_var);
					if (cpp_code.size() > 1)
						printf("%s%s; \t\t//%s\n", print_indexing, cpp_code.c_str(), DisassembledString.c_str());
				}
				catch (const std::exception&) { // didn't find stack trace. use base;
					printf("\033[1;31m%s%s; \t\t//%s -- didn't find trace -> use base\033[0m\n", print_indexing, AsmToCPP(instruction_trace[j].instruction, instruction_trace[j].rip, "baseModuleAddr").c_str(), DisassembledString.c_str());
					continue;
				}
			}
			else {
				std::string cpp_code = AsmToCPP(instruction_trace[j].instruction, instruction_trace[j].rip).c_str();

				if (cpp_code.size() > 1)
					printf("%s%s; \t\t//%s\n", print_indexing, cpp_code.c_str(), DisassembledString.c_str());
				else
					printf("%s\033[1;31m//failed to translate: %s\033[0m\n", print_indexing, DisassembledString.c_str());
			}
		}
	}
}

void Disassembler::Trace_Decryption(std::vector<InstructionTrace>& instruction_trace, ZydisRegister enc_reg)
{
	for (int32_t j = instruction_trace.size() - 1; j >= 0; j--)
	{
		if (instruction_trace[j].instruction.operands[0].reg.value == enc_reg)
		{
			instruction_trace[j].used = true;
			AddRequiredInstruction(instruction_trace, (instruction_trace.begin() + j));
			break;
		}
	}
}

void Disassembler::Load_DecryptionTrace(std::vector<InstructionTrace>& instruction_trace, uintptr_t decryption_end, ZydisMnemonic end_mnemonic)
{
	std::map<ZydisRegister, uint32_t> last_modified;
	std::map<int, uint32_t> rsp_stack_map;
	std::map<int, uint32_t> rbp_stack_map;
	instruction_trace.reserve(200);

	ZydisDecodedInstruction instruction = Decode(current_rip);
	while (current_rip != decryption_end && (end_mnemonic == ZydisMnemonic::ZYDIS_MNEMONIC_INVALID || instruction.mnemonic != end_mnemonic))
	{
		uintptr_t rip = debugger->SingleStep();

		instruction_trace.push_back({ instruction, last_modified, rsp_stack_map, rbp_stack_map, current_rip, debugger->GetContext(), false });

#ifdef DEBUG
		char DisassembledString[256];
		ZydisFormatterFormatInstruction(&formatter, &instruction, DisassembledString, sizeof(DisassembledString), 0);
		printf("read line %d: %s\n", instruction_trace.size() - 1, DisassembledString);
#endif
		ZydisRegister modified[8] = { ZydisRegister::ZYDIS_REGISTER_NONE };
		ZydisRegister accessed[8] = { ZydisRegister::ZYDIS_REGISTER_NONE };
		GetModifiedRegisters(instruction, modified);
		GetAccessedRegisters(instruction, accessed);
		for (size_t j = 0; j < 8; j++)
		{
			if (modified[j] != ZydisRegister::ZYDIS_REGISTER_NONE)
				last_modified[modified[j]] = instruction_trace.size() - 1;
		}
		if (instruction.operands[0].mem.base == ZydisRegister::ZYDIS_REGISTER_RSP) {
			for (size_t j = 0; j < 8; j++)
			{
				if (accessed[j] != ZydisRegister::ZYDIS_REGISTER_NONE) {
					try
					{
						rsp_stack_map[instruction.operands[0].mem.disp.value] = last_modified.at(accessed[j]);
					}
					catch (const std::exception&)
					{

					}
				}
			}
		}
		if (instruction.operands[0].mem.base == ZydisRegister::ZYDIS_REGISTER_RBP) {
			for (size_t j = 0; j < 8; j++)
			{
				if (accessed[j] != ZydisRegister::ZYDIS_REGISTER_NONE) {
					try
					{
						rbp_stack_map[instruction.operands[0].mem.disp.value] = last_modified.at(accessed[j]);
					}
					catch (const std::exception&)
					{

					}
				}
			}
		}

		current_rip = rip;
		if (debugger->exception_hit) {
			current_rip += instruction.length; //if exception is caused the ptr is not advanced.
			debugger->SetRIP(current_rip);
			debugger->exception_hit = false;
		}

		instruction = Decode(current_rip);
	}
}

void Disassembler::Dump_Decryption(uintptr_t decryption_end, ZydisRegister enc_reg, const char* print_indexing, ZydisMnemonic end_mnemonic)
{
	std::vector<InstructionTrace> instruction_trace;

	Load_DecryptionTrace(instruction_trace, decryption_end, end_mnemonic);

	Trace_Decryption(instruction_trace, enc_reg);

	Print_Decryption(instruction_trace, enc_reg, print_indexing);
}

void Disassembler::Dump_Switch()
{
	ZydisDecodedInstruction encrypted_read_instruction = Decode(current_rip);

	Print_PEB();

	SkipUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);
	ZydisDecodedInstruction jmp_to_end = Decode(current_rip);
	uintptr_t decryption_end = jmp_to_end.operands[0].imm.value.u + current_rip + jmp_to_end.length;
	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);

	Dump_Decryption(0, ZydisRegister::ZYDIS_REGISTER_MAX_VALUE, "\t", ZydisMnemonic::ZYDIS_MNEMONIC_AND);

	SkipUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_CMP);
	ZydisRegister switch_register = To64BitRegister(Decode(current_rip).operands[0].reg.value);
	uintptr_t switch_address = current_rip;
	SkipUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_ADD);
	ZydisRegister base_register = To64BitRegister(Decode(current_rip).operands[1].reg.value);

	printf("\t%s &= 0xF;\n\tswitch(%s) {\n", Get64BitRegisterString(switch_register).c_str(), Get64BitRegisterString(switch_register).c_str(), Get64BitRegisterString(switch_register).c_str());
	for (uint32_t i = 0; i < 16; i++)
	{
		printf("\tcase %d:\n\t{\n", i);

		current_rip = switch_address;
		debugger->SetRIP(current_rip);
		debugger->SetRegisterValue(switch_register, i);
		debugger->SetRegisterValue(base_register, debugger->base_address);

		Dump_Decryption(decryption_end, encrypted_read_instruction.operands[0].reg.value, "\t\t");

		printf("\t\treturn %s;\n\t}\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());
	}
	printf("\t}\n}\n");
}

void Disassembler::PrintRegisters()
{
	printf("\tconst uint64_t mb = baseModuleAddr;\n");
	printf("\tuint64_t rax = mb, rbx = mb, rcx = mb, rdx = mb, rdi = mb, rsi = mb, r8 = mb, r9 = mb, r10 = mb, r11 = mb, r12 = mb, r13 = mb, r14 = mb, r15 = mb;\n");
}

void Disassembler::Dump_ClientInfo_MW(uintptr_t address)
{
	ContextRestorer restorer(debugger);
	if (!address) {
		printf("//ClientInfo pattern scan failed.\n");
		return;
	}

	current_rip = address;
	printf("uintptr_t decrypt_client_info(const Driver& driver)\n{\n");
	PrintRegisters();

	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_TEST);
	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_MOV);

	ZydisDecodedInstruction encrypted_read_instruction = Decode(current_rip);
	ignore_trace.push_back(encrypted_read_instruction.operands[0].reg.value);
	printf("\t%s;\n", AsmToCPP(encrypted_read_instruction, current_rip).c_str());
	printf("\tif(!%s)\n\t\treturn %s;\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str(), Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());

	if (!Print_PEB()) {
		printf("\t//Failed to find peb. exiting\n}\n");
		return;
	}
	RunUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);
	ZydisDecodedInstruction jmp_to_end = Decode(current_rip);
	uintptr_t decryption_end = jmp_to_end.operands[0].imm.value.u + current_rip + jmp_to_end.length;
	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);

	Dump_Decryption(decryption_end, encrypted_read_instruction.operands[0].reg.value, "\t");
	printf("\treturn %s;\n}\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());
	ignore_trace.clear();
}

void Disassembler::Dump_ClientInfo_Vanguard(uintptr_t address)
{
	ContextRestorer restorer(debugger);
	if (!address) {
		printf("//ClientInfo pattern scan failed.\n");
		return;
	}

	current_rip = address;
	printf("uintptr_t decrypt_client_info(const Driver& driver)\n{\n");
	PrintRegisters();

	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);

	ZydisDecodedInstruction encrypted_read_instruction = Decode(current_rip);
	ignore_trace.push_back(encrypted_read_instruction.operands[0].reg.value);
	printf("\t%s;\n", AsmToCPP(encrypted_read_instruction, current_rip).c_str());
	printf("\tif(!%s)\n\t\treturn %s;\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str(), Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());

	RunUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);
	ZydisDecodedInstruction jmp_to_end = Decode(current_rip);
	uintptr_t decryption_end = jmp_to_end.operands[0].imm.value.u + current_rip + jmp_to_end.length;
	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);

	Dump_Decryption(decryption_end, encrypted_read_instruction.operands[0].reg.value, "\t");
	printf("\treturn %s;\n}\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());
	ignore_trace.clear();
}

void Disassembler::Dump_ClientBase(uintptr_t address)
{
	ContextRestorer restorer(debugger);
	if (!address) {
		printf("//ClientBase pattern scan failed.\n");
		return;
	}

	current_rip = address;

	printf("uintptr_t decrypt_client_base(const Driver& driver, uintptr_t client_info)\n{\n");
	PrintRegisters();

	ZydisDecodedInstruction encrypted_read_instruction = Decode(current_rip);
	ignore_trace.push_back(encrypted_read_instruction.operands[0].reg.value);
	std::string enc_client_info = AsmToCPP(encrypted_read_instruction, current_rip);
	printf("\t%s;\n", std::regex_replace(enc_client_info, std::regex(Get64BitRegisterString(encrypted_read_instruction.operands[1].mem.base)), "client_info").c_str());
	printf("\tif(!%s)\n\t\treturn %s;\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str(), Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());

	Dump_Switch();
	ignore_trace.clear();
}

void Disassembler::Dump_BoneBase(uintptr_t address)
{
	ContextRestorer restorer(debugger);
	if (!address) {
		printf("//BoneBase pattern scan failed.\n");
		return;
	}

	current_rip = address;
	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);

	printf("uintptr_t decrypt_bone_base(const Driver& driver)\n{\n");
	PrintRegisters();

	ZydisDecodedInstruction encrypted_read_instruction = Decode(current_rip);
	ignore_trace.push_back(encrypted_read_instruction.operands[0].reg.value);
	printf("\t%s;\n", AsmToCPP(encrypted_read_instruction, current_rip).c_str());
	printf("\tif(!%s)\n\t\treturn %s;\n", Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str(), Get64BitRegisterString(encrypted_read_instruction.operands[0].reg.value).c_str());

	Dump_Switch();
	ignore_trace.clear();
}

void Disassembler::Dump_BoneIndex(uintptr_t address)
{
	ContextRestorer restorer(debugger);
	if (!address) {
		printf("//BoneIndex pattern scan failed.\n");
		return;
	}

	current_rip = address;
	printf("uint16_t get_bone_index(const Driver& driver, uint32_t bone_index)\n{\n");
	PrintRegisters();

	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);
	SkipUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_TEST);
	ZydisRegister return_register = Decode(current_rip).operands[0].reg.value;
	current_rip = address;

	SkipOverUntilInstruction(ZydisMnemonic::ZYDIS_MNEMONIC_JZ);

	ZydisDecodedInstruction instruction = Decode(current_rip);
	printf("\t%s = bone_index;\n", Get64BitRegisterString(instruction.operands[1].reg.value).c_str());
	printf("\t%s;\n", AsmToCPP(instruction, current_rip).c_str());
	ignore_trace.push_back(instruction.operands[0].reg.value);

	current_rip = debugger->SingleStep();
	if (debugger->exception_hit) {
		current_rip += instruction.length; //if exception is caused the ptr is not advanced.
		debugger->SetRIP(current_rip);
		debugger->exception_hit = false;
	}
	Dump_Decryption(0, return_register, "\t", ZydisMnemonic::ZYDIS_MNEMONIC_TEST);
	printf("\treturn %s;\n}", Get64BitRegisterString(return_register).c_str());
	ignore_trace.clear();
}
void Disassembler::Dump_Offsets_MW()
{
	{
		uintptr_t addr = debugger->scanner->Find_Pattern("33 05 ? ? ? ? 89 44 24 34 48 8B 44 24");
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto ref_def_ptr = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address - 0x4);
		else
			printf("\t\033[1;31mstatic constexpr auto refdef = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8D 0D ? ? ? ? 48 8B 0C D1 8B D3 48 8B 01 FF 90 ? ? ? ? ");
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto name_array = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address);
		else
			printf("\t\033[1;31mstatic constexpr auto name_array = 0x0;\033[0m\n");
		printf("\tstatic constexpr auto name_array_pos = 0x5E70; // 0x4C70 for MW1(2019)\n");
		printf("\tstatic constexpr auto name_array_size = 0xD0;\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8D 05 ? ? ? ? 48 03 F8 80 BF");
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto loot_ptr = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address - 0x4);
		else
			printf("\t\033[1;31mstatic constexpr auto loot = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8B 05 ? ? ? ? 48 8B 7C 24 ? 48 05 ? ? ? ? 48 69 CA ? ? ? ? 48 03 C1 C3");
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto camera_base = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address);
		else
			printf("\t\033[1;31mstatic constexpr auto camera_base = 0x0;\033[0m\n");
		printf("\tstatic constexpr auto camera_pos = 0x1D8;\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8B 83 ? ? ? ? 4C 8D 45 ? 48 8D 4C 24 ? 8B 50 0C", true);
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RBX && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto local_index = 0x%llX;\n", instruction.operands[1].mem.disp.value);
		else
			printf("\t\033[1;31mstatic constexpr auto local_index = 0x0;\033[0m\n");
		printf("\tstatic constexpr auto local_index_pos = 0x2CC; // 0x1FC for MW1 (2019)\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("41 8B 52 0C 4D 8D 4A 04 4D 8D 42 08 4C 89 95 ? ? ? ? 8B C2 4C 89 8D") - 0x9;//DEAD
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RSI && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto recoil = 0x%llX;\n", instruction.operands[1].mem.disp.value);
		else
			printf("\t\033[1;31mstatic constexpr auto recoil = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("3B 0D ?? ?? ?? ?? 0F 47 0D ?? ?? ?? ?? 41 89 4D 24"); //3B 1D ? ? ? ? 89 5D 88 89 9D ? ? ? ? 0F 8D ? ? ? ? 48 8B 3D
		auto instruction = Decode(addr);
		if (instruction.operands[0].reg.value == ZYDIS_REGISTER_ECX &&instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto game_mode = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address);
		else
			printf("\t\033[1;31mstatic constexpr auto game_mode = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8D 1D ?? ?? ?? ?? B2 01 8B 00 0F B7 C8 48 8B 1C CB");
		auto instruction = Decode(addr);
		if (instruction.operands[0].reg.value == ZYDIS_REGISTER_RBX &&instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto weapon_definitions = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address);
		else
			printf("\t\033[1;31mstatic constexpr auto weapon_definitions = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8B 1D ?? ?? ?? ?? BA FF FF FF FF 48 8B CF E8");
		auto instruction = Decode(addr);
		if (instruction.operands[0].reg.value == ZYDIS_REGISTER_RBX && instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto distribute = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address);
		else
			printf("\t\033[1;31mstatic constexpr auto distribute = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("80 BF ? ? ? ? ? 74 17 3B 87");
		auto instruction = Decode(addr);
		if (instruction.operands[0].mem.base == ZYDIS_REGISTER_RDI && instruction.operands[0].mem.disp.has_displacement)
			printf("\tstatic constexpr auto visible_offset = 0x%llX;\n", instruction.operands[0].mem.disp.value);
		else
			printf("\t\033[1;31mstatic constexpr auto visible_offset = 0x0;\033[0m\n");
	}

	{
		uintptr_t addr = debugger->scanner->Find_Pattern("48 8D 05 ? ? ? ? 49 8B CE 48 89 47 38 C5 FA 11 87");
		auto instruction = Decode(addr);
		if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RIP && instruction.operands[1].mem.disp.has_displacement)
			printf("\tstatic constexpr auto visible = 0x%llX;\n", (addr + instruction.operands[1].mem.disp.value + instruction.length) - debugger->base_address);
		else
			printf("\t\033[1;31mstatic constexpr auto visible = 0x0;\033[0m\n");
	}

	printf("\n");

	printf("\tclass bone {\n\tpublic:\n");
	{
		{
			uintptr_t addr = debugger->scanner->Find_Pattern("C4 C1 7A 58 88 ? ? ? ? C5 FA 11 8A ? ? ? ? C5 FA 10 51 ? C4 C1 6A 58 80 ? ? ? ? C5 FA 11 82 ? ? ? ? C5 FA 10 49 ? C4 C1 72 58 90");
			auto instruction = Decode(addr);
			if (instruction.operands[2].mem.base == ZYDIS_REGISTER_R8 && instruction.operands[2].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto bone_base = 0x%llX;\n", instruction.operands[2].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto bone_base = 0x0;\033[0m\n");
			printf("\t\tstatic constexpr auto size = 0x180; //0x150 for MW1(2019).\n");
			printf("\t\tstatic constexpr auto offset = 0xD8; //0xC0 for MW1(2019).\n");
		}
	}
	printf("\t};\n");

	printf("\n");

	printf("\tclass player {\n\tpublic:\n");
	{
		{
			uintptr_t addr = debugger->scanner->Find_Pattern("49 63 C7 48 69 D8 ? ? ? ? 48 03 DA ") + 3;
			auto instruction = Decode(addr);
			if (instruction.operands[2].type == ZydisOperandType::ZYDIS_OPERAND_TYPE_IMMEDIATE)
				printf("\t\tstatic constexpr auto size = 0x%llX;\n", instruction.operands[2].imm.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto size = 0x0;\033[0m\n");
		}

		{
			uintptr_t addr = debugger->scanner->Find_Pattern("7D ? 41 38 ? ? ? ? ? 74") + 2;
			auto instruction = Decode(addr);
			if (instruction.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && instruction.operands[0].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto valid = 0x%llX;\n", instruction.operands[0].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto valid = 0x0;\033[0m\n");
		}

		{
			uintptr_t addr = debugger->scanner->Find_Pattern("48 8B 8B ? ? ? ? 48 39 01 74 2F 33 D2 C6 83 ? ? ? ? ? C6 83 ? ? ? ? ? E8");
			auto instruction = Decode(addr);
			if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RBX && instruction.operands[1].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto pos = 0x%llX;\n", instruction.operands[1].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto pos = 0x0;\033[0m\n");
		}

		{
			uintptr_t addr = debugger->scanner->Find_Pattern("3A 88 ? ? ? ? 0F 84 ? ? ? ? 80 3D");
			auto instruction = Decode(addr);
			if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RAX && instruction.operands[1].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto team = 0x%llX;\n", instruction.operands[1].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto team = 0x0;\033[0m\n");
		}

		{
			uintptr_t addr = debugger->scanner->Find_Pattern("0F 48 F0 83 BF ? ? ? ? ? 75 0A F3 0F 10 35 ? ? ? ? EB 08");
			auto instruction = Decode(addr);
			instruction = Decode(addr + instruction.length);
			if (instruction.operands[0].mem.base == ZYDIS_REGISTER_RDI && instruction.operands[0].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto stance = 0x%llX;\n", instruction.operands[0].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto stance = 0x0;\033[0m\n");
		}

		{
			uintptr_t addr = debugger->scanner->Find_Pattern("4C 8D 85 ? ? ? ? 48 8B F8 4C 8B 08 41 FF 51 08");//DEAD
			auto instruction = Decode(addr);
			if (instruction.operands[1].mem.base == ZYDIS_REGISTER_RBP && instruction.operands[1].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto weapon_index = 0x%llX;\n", instruction.operands[1].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto weapon_index = 0x0;\033[0m\n");
		}

		{
			uintptr_t addr = debugger->scanner->Find_Pattern("33 D2 C6 83 ? ? ? ? ? C6 83 ? ? ? ? ? E8 ? ? ? ? 44 0F B6 C6 48 8B D5 48 8B CF E8") + 2;
			auto instruction = Decode(addr);
			if (instruction.operands[0].mem.base == ZYDIS_REGISTER_RBX && instruction.operands[0].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto dead_1 = 0x%llX;\n", instruction.operands[0].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto dead_1 = 0x0;\033[0m\n");
			instruction = Decode(addr + instruction.length);
			if (instruction.operands[0].mem.base == ZYDIS_REGISTER_RBX && instruction.operands[0].mem.disp.has_displacement)
				printf("\t\tstatic constexpr auto dead_2 = 0x%llX;\n", instruction.operands[0].mem.disp.value);
			else
				printf("\t\t\033[1;31mstatic constexpr auto dead_2 = 0x0;\033[0m\n");
		}
	}
	printf("\t};\n\n");
}
