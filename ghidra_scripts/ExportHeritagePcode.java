// Export decompiler P-Code after Ghidra has run its function-level analysis.
//@category PCode

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.pcode.FunctionPrototype;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.pcode.HighVariable;
import ghidra.program.model.pcode.PcodeBlock;
import ghidra.program.model.pcode.PcodeBlockBasic;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.SequenceNumber;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.pcode.VarnodeAST;

public class ExportHeritagePcode extends GhidraScript {

	private final Map<String, Varnode> varnodes = new LinkedHashMap<>();
	private int registerVarnodeCount = 0;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			println("usage: ExportHeritagePcode.java <output.json> [function-entry-or-name] [style] [timeout-sec]");
			return;
		}

		Function function = resolveFunction(args.length >= 2 ? args[1] : "");
		if (function == null) {
			throw new IllegalArgumentException("cannot resolve function: " +
				(args.length >= 2 ? args[1] : "<current-address>"));
		}

		String style = args.length >= 3 ? args[2] : "decompile";
		int timeoutSec = args.length >= 4 ? Integer.parseInt(args[3]) : 60;
		HighFunction highFunction = decompile(function, style, timeoutSec);

		File output = new File(args[0]);
		File parent = output.getParentFile();
		if (parent != null) {
			parent.mkdirs();
		}
		try (PrintWriter out = new PrintWriter(new FileWriter(output))) {
			writeJson(out, highFunction, style);
		}

		println("exported " + function.getName() + " to " + output.getAbsolutePath());
		println("register-space varnodes: " + registerVarnodeCount);
	}

	private Function resolveFunction(String target) {
		if (target == null || target.isEmpty()) {
			if (currentAddress == null) {
				return null;
			}
			return getFunctionContaining(currentAddress);
		}

		try {
			Address address = toAddr(target);
			Function atAddress = currentProgram.getFunctionManager().getFunctionAt(address);
			if (atAddress != null) {
				return atAddress;
			}
			return currentProgram.getFunctionManager().getFunctionContaining(address);
		}
		catch (Exception ignored) {
			// Not an address. Try function name below.
		}

		FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
		while (functions.hasNext()) {
			Function function = functions.next();
			if (function.getName().equals(target) ||
				function.getEntryPoint().toString().equals(target) ||
				function.getEntryPoint().toString(true).equals(target)) {
				return function;
			}
		}
		return null;
	}

	private HighFunction decompile(Function function, String style, int timeoutSec) throws Exception {
		DecompInterface decompiler = new DecompInterface();
		try {
			DecompileOptions options = new DecompileOptions();
			decompiler.setOptions(options);
			decompiler.toggleCCode(false);
			decompiler.toggleSyntaxTree(true);
			decompiler.setSimplificationStyle(style);
			if (!decompiler.openProgram(currentProgram)) {
				throw new IllegalStateException("decompiler open failed: " + decompiler.getLastMessage());
			}
			DecompileResults results = decompiler.decompileFunction(function, timeoutSec, monitor);
			HighFunction highFunction = results.getHighFunction();
			if (highFunction == null) {
				throw new IllegalStateException("decompiler produced no HighFunction: " +
					results.getErrorMessage());
			}
			return highFunction;
		}
		finally {
			decompiler.dispose();
		}
	}

	private void writeJson(PrintWriter out, HighFunction highFunction, String style) {
		collectVarnodes(highFunction);
		registerVarnodeCount = countRegisterVarnodes();

		out.println("{");
		writeProgram(out, highFunction, style);
		out.println(",");
		writeFunction(out, highFunction);
		out.println(",");
		writeBlocks(out, highFunction);
		out.println(",");
		writeOps(out, highFunction);
		out.println(",");
		writeVarnodes(out);
		out.println(",");
		writeStats(out, highFunction);
		out.println();
		out.println("}");
	}

	private void collectVarnodes(HighFunction highFunction) {
		Iterator<PcodeOpAST> ops = highFunction.getPcodeOps();
		while (ops.hasNext()) {
			PcodeOp op = ops.next();
			remember(op.getOutput());
			for (int i = 0; i < op.getNumInputs(); ++i) {
				remember(op.getInput(i));
			}
		}

		FunctionPrototype prototype = highFunction.getFunctionPrototype();
		if (prototype != null) {
			for (int i = 0; i < prototype.getNumParams(); ++i) {
				rememberRepresentative(prototype.getParam(i));
			}
		}
	}

	private void rememberRepresentative(HighSymbol symbol) {
		if (symbol == null) {
			return;
		}
		HighVariable highVariable = symbol.getHighVariable();
		if (highVariable == null) {
			return;
		}
		remember(highVariable.getRepresentative());
	}

	private void remember(Varnode varnode) {
		if (varnode == null) {
			return;
		}
		varnodes.putIfAbsent(vnodeId(varnode), varnode);
	}

	private int countRegisterVarnodes() {
		int count = 0;
		for (Varnode varnode : varnodes.values()) {
			if (varnode.isRegister()) {
				count++;
			}
		}
		return count;
	}

	private void writeProgram(PrintWriter out, HighFunction highFunction, String style) {
		out.println("  \"schema\": \"notdec.heritage-pcode.v0\",");
		out.println("  \"program\": {");
		out.println("    \"name\": " + json(currentProgram.getName()) + ",");
		out.println("    \"language\": " + json(highFunction.getLanguage().toString()) + ",");
		out.println("    \"compilerSpec\": " + json(highFunction.getCompilerSpec().toString()) + ",");
		out.println("    \"simplificationStyle\": " + json(style));
		out.print("  }");
	}

	private void writeFunction(PrintWriter out, HighFunction highFunction) {
		Function function = highFunction.getFunction();
		FunctionPrototype prototype = highFunction.getFunctionPrototype();
		out.println("  \"function\": {");
		out.println("    \"name\": " + json(function.getName()) + ",");
		out.println("    \"entry\": " + json(addressString(function.getEntryPoint())) + ",");
		out.println("    \"callingConvention\": " + json(function.getCallingConventionName()) + ",");
		out.println("    \"returnType\": " + json(typeString(prototype != null ? prototype.getReturnType() : null)) + ",");
		out.println("    \"params\": [");
		if (prototype != null) {
			for (int i = 0; i < prototype.getNumParams(); ++i) {
				if (i != 0) {
					out.println(",");
				}
				writeParam(out, highFunction, prototype.getParam(i), i);
			}
		}
		out.println();
		out.println("    ]");
		out.print("  }");
	}

	private void writeParam(PrintWriter out, HighFunction highFunction, HighSymbol symbol, int index) {
		out.print("      {");
		out.print("\"index\": " + index);
		out.print(", \"name\": " + json(symbol.getName()));
		out.print(", \"type\": " + json(typeString(symbol.getDataType())));
		out.print(", \"storage\": " + json(symbol.getStorage().toString()));
		HighVariable highVariable = symbol.getHighVariable();
		if (highVariable != null && highVariable.getRepresentative() != null) {
			out.print(", \"varnode\": " + json(vnodeId(highVariable.getRepresentative())));
			out.print(", \"highVariable\": " + json(highVariable.getName()));
		}
		out.print("}");
	}

	private void writeBlocks(PrintWriter out, HighFunction highFunction) {
		out.println("  \"blocks\": [");
		boolean first = true;
		for (PcodeBlockBasic block : highFunction.getBasicBlocks()) {
			if (!first) {
				out.println(",");
			}
			first = false;
			out.println("    {");
			out.println("      \"id\": " + json(blockId(block)) + ",");
			out.println("      \"index\": " + block.getIndex() + ",");
			out.println("      \"start\": " + json(addressString(block.getStart())) + ",");
			writeBlockRefs(out, "in", block, true);
			out.println(",");
			writeBlockRefs(out, "out", block, false);
			out.println(",");
			out.println("      \"ops\": [");
			Iterator<PcodeOp> ops = block.getIterator();
			boolean firstOp = true;
			while (ops.hasNext()) {
				PcodeOp op = ops.next();
				if (!firstOp) {
					out.println(",");
				}
				firstOp = false;
				out.print("        " + json(opId(op)));
			}
			out.println();
			out.println("      ]");
			out.print("    }");
		}
		out.println();
		out.print("  ]");
	}

	private void writeBlockRefs(PrintWriter out, String name, PcodeBlockBasic block, boolean incoming) {
		out.print("      \"" + name + "\": [");
		int count = incoming ? block.getInSize() : block.getOutSize();
		for (int i = 0; i < count; ++i) {
			if (i != 0) {
				out.print(", ");
			}
			out.print(json(blockId(incoming ? block.getIn(i) : block.getOut(i))));
		}
		out.print("]");
	}

	private void writeOps(PrintWriter out, HighFunction highFunction) {
		out.println("  \"ops\": [");
		Iterator<PcodeOpAST> ops = highFunction.getPcodeOps();
		boolean first = true;
		while (ops.hasNext()) {
			PcodeOp op = ops.next();
			if (!first) {
				out.println(",");
			}
			first = false;
			writeOp(out, op);
		}
		out.println();
		out.print("  ]");
	}

	private void writeOp(PrintWriter out, PcodeOp op) {
		SequenceNumber seq = op.getSeqnum();
		out.println("    {");
		out.println("      \"id\": " + json(opId(op)) + ",");
		out.println("      \"parent\": " + json(blockId(op.getParent())) + ",");
		out.println("      \"seqTarget\": " + json(seq != null ? addressString(seq.getTarget()) : null) + ",");
		out.println("      \"seqTime\": " + (seq != null ? seq.getTime() : -1) + ",");
		out.println("      \"opcode\": " + op.getOpcode() + ",");
		out.println("      \"mnemonic\": " + json(op.getMnemonic()) + ",");
		out.println("      \"text\": " + json(op.toString()) + ",");
		out.println("      \"callTarget\": " + json(directCallTarget(op)) + ",");
		out.println("      \"callTargetName\": " + json(directCallTargetName(op)) + ",");
		out.println("      \"output\": " + json(vnodeId(op.getOutput())) + ",");
		out.println("      \"inputs\": [");
		for (int i = 0; i < op.getNumInputs(); ++i) {
			if (i != 0) {
				out.println(",");
			}
			out.print("        " + json(vnodeId(op.getInput(i))));
		}
		out.println();
		out.println("      ]");
		out.print("    }");
	}

	private void writeVarnodes(PrintWriter out) {
		out.println("  \"varnodes\": [");
		boolean first = true;
		for (Varnode varnode : varnodes.values()) {
			if (!first) {
				out.println(",");
			}
			first = false;
			writeVarnode(out, varnode);
		}
		out.println();
		out.print("  ]");
	}

	private void writeVarnode(PrintWriter out, Varnode varnode) {
		HighVariable highVariable = varnode.getHigh();
		out.println("    {");
		out.println("      \"id\": " + json(vnodeId(varnode)) + ",");
		out.println("      \"space\": " + json(varnode.getAddress().getAddressSpace().getName()) + ",");
		out.println("      \"offset\": " + json(Long.toUnsignedString(varnode.getOffset())) + ",");
		out.println("      \"size\": " + varnode.getSize() + ",");
		out.println("      \"address\": " + json(addressString(varnode.getAddress())) + ",");
		out.println("      \"pcAddress\": " + json(addressString(varnode.getPCAddress())) + ",");
		out.println("      \"registerName\": " + json(registerName(varnode)) + ",");
		out.println("      \"isInput\": " + varnode.isInput() + ",");
		out.println("      \"isRegister\": " + varnode.isRegister() + ",");
		out.println("      \"isUnique\": " + varnode.isUnique() + ",");
		out.println("      \"isConstant\": " + varnode.isConstant() + ",");
		out.println("      \"isAddressTied\": " + varnode.isAddrTied() + ",");
		out.println("      \"highVariable\": " + json(highVariable != null ? highVariable.getName() : null) + ",");
		out.println("      \"highType\": " + json(highVariable != null ? typeString(highVariable.getDataType()) : null) + ",");
		out.println("      \"def\": " + json(varnode.getDef() != null ? opId(varnode.getDef()) : null));
		out.print("    }");
	}

	private void writeStats(PrintWriter out, HighFunction highFunction) {
		int opCount = 0;
		int multiequalCount = 0;
		Iterator<PcodeOpAST> ops = highFunction.getPcodeOps();
		while (ops.hasNext()) {
			PcodeOp op = ops.next();
			opCount++;
			if (op.getOpcode() == PcodeOp.MULTIEQUAL) {
				multiequalCount++;
			}
		}

		out.println("  \"stats\": {");
		out.println("    \"blockCount\": " + highFunction.getBasicBlocks().size() + ",");
		out.println("    \"opCount\": " + opCount + ",");
		out.println("    \"varnodeCount\": " + varnodes.size() + ",");
		out.println("    \"registerVarnodeCount\": " + registerVarnodeCount + ",");
		out.println("    \"multiequalCount\": " + multiequalCount);
		out.print("  }");
	}

	private String opId(PcodeOp op) {
		if (op == null) {
			return null;
		}
		SequenceNumber seq = op.getSeqnum();
		if (seq == null) {
			return "op:unknown:" + System.identityHashCode(op);
		}
		return "op:" + addressString(seq.getTarget()) + ":" + seq.getTime();
	}

	private String vnodeId(Varnode varnode) {
		if (varnode == null) {
			return null;
		}
		if (varnode instanceof VarnodeAST) {
			return "vn:" + ((VarnodeAST) varnode).getUniqueId();
		}
		return "vn:" + varnode.getAddress().getAddressSpace().getName() + ":" +
			Long.toUnsignedString(varnode.getOffset()) + ":" + varnode.getSize();
	}

	private String blockId(PcodeBlockBasic block) {
		if (block == null) {
			return null;
		}
		return "bb:" + block.getIndex();
	}

	private String blockId(PcodeBlock block) {
		if (block == null) {
			return null;
		}
		return "bb:" + block.getIndex();
	}

	private String addressString(Address address) {
		if (address == null || address == Address.NO_ADDRESS) {
			return null;
		}
		return address.toString(true);
	}

	private String registerName(Varnode varnode) {
		if (!varnode.isRegister()) {
			return null;
		}
		Register register = currentProgram.getRegister(varnode.getAddress(), varnode.getSize());
		return register != null ? register.getName() : null;
	}

	private String directCallTarget(PcodeOp op) {
		Address address = directCallTargetAddress(op);
		return address != null ? addressString(address) : null;
	}

	private Address directCallTargetAddress(PcodeOp op) {
		if (op.getOpcode() != PcodeOp.CALL || op.getNumInputs() == 0) {
			return null;
		}
		Varnode target = op.getInput(0);
		return target != null ? target.getAddress() : null;
	}

	private String directCallTargetName(PcodeOp op) {
		Address target = directCallTargetAddress(op);
		if (target == null) {
			return null;
		}
		Function function = currentProgram.getFunctionManager().getFunctionAt(target);
		return function != null ? function.getName() : null;
	}

	private String typeString(DataType type) {
		return type != null ? type.toString() : null;
	}

	private String json(String value) {
		if (value == null) {
			return "null";
		}
		StringBuilder out = new StringBuilder();
		out.append('"');
		for (int i = 0; i < value.length(); ++i) {
			char c = value.charAt(i);
			switch (c) {
				case '"':
					out.append("\\\"");
					break;
				case '\\':
					out.append("\\\\");
					break;
				case '\b':
					out.append("\\b");
					break;
				case '\f':
					out.append("\\f");
					break;
				case '\n':
					out.append("\\n");
					break;
				case '\r':
					out.append("\\r");
					break;
				case '\t':
					out.append("\\t");
					break;
				default:
					if (c < 0x20) {
						out.append(String.format("\\u%04x", (int) c));
					}
					else {
						out.append(c);
					}
					break;
			}
		}
		out.append('"');
		return out.toString();
	}
}
