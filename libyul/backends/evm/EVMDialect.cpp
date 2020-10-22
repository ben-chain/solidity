/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Yul dialects for EVM.
 */

#include <libyul/backends/evm/EVMDialect.h>

#include <libyul/AsmAnalysisInfo.h>
#include <libyul/AsmData.h>
#include <libyul/Object.h>
#include <libyul/Exceptions.h>
#include <libyul/AsmParser.h>
#include <libyul/backends/evm/AbstractAssembly.h>

#include <libevmasm/SemanticInformation.h>
#include <libevmasm/Instruction.h>

#include <liblangutil/Exceptions.h>

#include <boost/range/adaptor/reversed.hpp>

using namespace std;
using namespace dev;
using namespace yul;

namespace
{
pair<YulString, BuiltinFunctionForEVM> createEVMFunction(
	string const& _name,
	dev::eth::Instruction _instruction
)
{
	eth::InstructionInfo info = dev::eth::instructionInfo(_instruction);
	BuiltinFunctionForEVM f;
	f.name = YulString{_name};
	f.parameters.resize(info.args);
	f.returns.resize(info.ret);
	f.sideEffects = EVMDialect::sideEffectsOfInstruction(_instruction);
	f.isMSize = _instruction == dev::eth::Instruction::MSIZE;
	f.literalArguments = false;
	f.instruction = _instruction;
	f.generateCode = [_instruction](
		FunctionCall const&,
		AbstractAssembly& _assembly,
		BuiltinContext&,
		std::function<void()> _visitArguments
	) {
		_visitArguments();
		_assembly.appendInstruction(_instruction);
	};

	return {f.name, move(f)};
}

pair<YulString, BuiltinFunctionForEVM> createFunction(
	string _name,
	size_t _params,
	size_t _returns,
	SideEffects _sideEffects,
	bool _literalArguments,
	std::function<void(FunctionCall const&, AbstractAssembly&, BuiltinContext&, std::function<void()>)> _generateCode
)
{
	YulString name{std::move(_name)};
	BuiltinFunctionForEVM f;
	f.name = name;
	f.parameters.resize(_params);
	f.returns.resize(_returns);
	f.sideEffects = std::move(_sideEffects);
	f.literalArguments = _literalArguments;
	f.isMSize = false;
	f.instruction = {};
	f.generateCode = std::move(_generateCode);
	return {name, f};
}

map<YulString, BuiltinFunctionForEVM> createBuiltins(langutil::EVMVersion _evmVersion, bool _objectAccess)
{
	map<YulString, BuiltinFunctionForEVM> builtins;
	for (auto const& instr: Parser::instructions())
		if (
			!dev::eth::isDupInstruction(instr.second) &&
			!dev::eth::isSwapInstruction(instr.second) &&
			instr.second != eth::Instruction::JUMP &&
			instr.second != eth::Instruction::JUMPI &&
			_evmVersion.hasOpcode(instr.second)
		)
			builtins.emplace(createEVMFunction(instr.first, instr.second));

	builtins.emplace(createFunction(
			"kall",
			4,
			0,
			SideEffects{false, false, false, false, true},
			false,
			[](
				FunctionCall const&,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				std::function<void()> _visitArguments
			) {
				_visitArguments();

				_assembly.appendInstruction(dev::eth::Instruction::CALLER);
				_assembly.appendConstant(0);
				_assembly.appendInstruction(dev::eth::Instruction::SWAP1);
				_assembly.appendInstruction(dev::eth::Instruction::GAS);
				_assembly.appendInstruction(dev::eth::Instruction::CALL);
				_assembly.appendInstruction(dev::eth::Instruction::PC);
				_assembly.appendConstant(29);
				_assembly.appendInstruction(dev::eth::Instruction::ADD);
				_assembly.appendInstruction(dev::eth::Instruction::JUMPI);

				_assembly.appendInstruction(dev::eth::Instruction::RETURNDATASIZE);
				_assembly.appendConstant(1);
				_assembly.appendInstruction(dev::eth::Instruction::EQ);
				_assembly.appendInstruction(dev::eth::Instruction::PC);
				_assembly.appendConstant(12);
				_assembly.appendInstruction(dev::eth::Instruction::ADD);

				_assembly.appendInstruction(dev::eth::Instruction::JUMPI);
				_assembly.appendInstruction(dev::eth::Instruction::RETURNDATASIZE);
				_assembly.appendConstant(0);
				_assembly.appendInstruction(dev::eth::Instruction::DUP1);
				_assembly.appendInstruction(dev::eth::Instruction::RETURNDATACOPY);
				_assembly.appendInstruction(dev::eth::Instruction::RETURNDATASIZE);
				// begin: changed ops from what we "really want"
				_assembly.appendConstant(1193046); // 0x123456, this should be PUSH1 0 in final form but accounts for the two missing jumpdests

				_assembly.appendInstruction(dev::eth::Instruction::MSTORE); // instead of REVERT
				// _assembly.appendInstruction(evmasm::Instruction::REVERT);

				// _assembly.appendLabel(newlabel1); // adds to appended constant

				// _assembly.appendConstant(1);
				// _assembly.appendConstant(0); // these are more likely to be optimized since subseqent usage will have 0 in args so optimizer tries to DUP it(us)
				_assembly.appendConstant(234);
				_assembly.appendConstant(156);
				_assembly.appendInstruction(dev::eth::Instruction::MSTORE); // instead of RETURN
			}
		));

		builtins.emplace(createFunction(
			"kopy",
			4,
			0,
			SideEffects{false, false, false, false, true},
			false,
			[](
				FunctionCall const&,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				std::function<void()> _visitArguments
			) {
				_visitArguments();
				_assembly.appendInstruction(dev::eth::Instruction::CALLER);
				_assembly.appendInstruction(dev::eth::Instruction::POP);
				_assembly.appendConstant(0);
				_assembly.appendConstant(4);
				_assembly.appendInstruction(dev::eth::Instruction::GAS);
				_assembly.appendInstruction(dev::eth::Instruction::CALL);
				_assembly.appendInstruction(dev::eth::Instruction::POP);
			}
		));

	if (_objectAccess)
	{
		builtins.emplace(createFunction("datasize", 1, 1, SideEffects{}, true, [](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext& _context,
			function<void()>
		) {
			yulAssert(_context.currentObject, "No object available.");
			yulAssert(_call.arguments.size() == 1, "");
			Expression const& arg = _call.arguments.front();
			YulString dataName = std::get<Literal>(arg).value;
			if (_context.currentObject->name == dataName)
				_assembly.appendAssemblySize();
			else
			{
				yulAssert(
					_context.subIDs.count(dataName) != 0,
					"Could not find assembly object <" + dataName.str() + ">."
				);
				_assembly.appendDataSize(_context.subIDs.at(dataName));
			}
		}));
		builtins.emplace(createFunction("dataoffset", 1, 1, SideEffects{}, true, [](
			FunctionCall const& _call,
			AbstractAssembly& _assembly,
			BuiltinContext& _context,
			std::function<void()>
		) {
			yulAssert(_context.currentObject, "No object available.");
			yulAssert(_call.arguments.size() == 1, "");
			Expression const& arg = _call.arguments.front();
			YulString dataName = std::get<Literal>(arg).value;
			if (_context.currentObject->name == dataName)
				_assembly.appendConstant(0);
			else
			{
				yulAssert(
					_context.subIDs.count(dataName) != 0,
					"Could not find assembly object <" + dataName.str() + ">."
				);
				_assembly.appendDataOffset(_context.subIDs.at(dataName));
			}
		}));
		builtins.emplace(createFunction(
			"datacopy",
			3,
			0,
			SideEffects{false, false, false, false, true},
			false,
			[](
				FunctionCall const&,
				AbstractAssembly& _assembly,
				BuiltinContext&,
				std::function<void()> _visitArguments
			) {
				_visitArguments();
				_assembly.appendInstruction(dev::eth::Instruction::CODECOPY);
			}
		));
	}
	return builtins;
}

}

EVMDialect::EVMDialect(AsmFlavour _flavour, bool _objectAccess, langutil::EVMVersion _evmVersion):
	Dialect{_flavour},
	m_objectAccess(_objectAccess),
	m_evmVersion(_evmVersion),
	m_functions(createBuiltins(_evmVersion, _objectAccess))
{
}

BuiltinFunctionForEVM const* EVMDialect::builtin(YulString _name) const
{
	auto it = m_functions.find(_name);
	if (it != m_functions.end())
		return &it->second;
	else
		return nullptr;
}

EVMDialect const& EVMDialect::looseAssemblyForEVM(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialect const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialect>(AsmFlavour::Loose, false, _version);
	return *dialects[_version];
}

EVMDialect const& EVMDialect::strictAssemblyForEVM(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialect const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialect>(AsmFlavour::Strict, false, _version);
	return *dialects[_version];
}

EVMDialect const& EVMDialect::strictAssemblyForEVMObjects(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialect const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialect>(AsmFlavour::Strict, true, _version);
	return *dialects[_version];
}

EVMDialect const& EVMDialect::yulForEVM(langutil::EVMVersion _version)
{
	static map<langutil::EVMVersion, unique_ptr<EVMDialect const>> dialects;
	static YulStringRepository::ResetCallback callback{[&] { dialects.clear(); }};
	if (!dialects[_version])
		dialects[_version] = make_unique<EVMDialect>(AsmFlavour::Yul, false, _version);
	return *dialects[_version];
}

SideEffects EVMDialect::sideEffectsOfInstruction(eth::Instruction _instruction)
{
	return SideEffects{
		eth::SemanticInformation::movable(_instruction),
		eth::SemanticInformation::sideEffectFree(_instruction),
		eth::SemanticInformation::sideEffectFreeIfNoMSize(_instruction),
		eth::SemanticInformation::invalidatesStorage(_instruction),
		eth::SemanticInformation::invalidatesMemory(_instruction)
	};
}
