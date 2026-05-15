// Export multiple Ghidra HighFunction heritage P-Code bodies into one module JSON.
//@category PCode

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.Iterator;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

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

public class ExportHeritageModule extends GhidraScript {

	private static class Options {
		String outputPath;
		int limit = 20;
		int timeoutSec = 60;
		String style = "decompile";
	}

	private static class Failure {
		String entry;
		String name;
		String stage;
		String message;
	}

	private final Map<String, Varnode> varnodes = new LinkedHashMap<>();
	private final Map<String, Function> externals = new LinkedHashMap<>();
	private final Set<String> seenEntries = new LinkedHashSet<>();
	private int registerVarnodeCount = 0;

	@Override
	public void run() throws Exception {
		Options options = parseOptions(getScriptArgs());
		if (options == null) {
			println("usage: ExportHeritageModule.java <output.json> [--all|--limit=N|N] [--timeout=N|timeout-sec] [--style=name]");
			return;
		}

		long startNanos = System.nanoTime();
		int attempted = 0;
		int succeeded = 0;
		Set<Failure> failures = new LinkedHashSet<>();

		File output = new File(options.outputPath);
		File parent = output.getParentFile();
		if (parent != null) {
			parent.mkdirs();
		}

		DecompInterface decompiler = createDecompiler(options.style);
		try (PrintWriter out = new PrintWriter(new FileWriter(output))) {
			List<Function> selectedFunctions = selectFunctions(options.limit);
			for (Function function : selectedFunctions) {
				seenEntries.add(addressString(function.getEntryPoint()));
			}

			out.println("{");
			writeProgram(out, options.style);
			out.println(",");
			out.println("  \"functions\": [");

			boolean firstFunction = true;
			for (Function function : selectedFunctions) {
				attempted++;
				try {
					HighFunction highFunction = decompile(decompiler, function, options.timeoutSec);
					if (!firstFunction) {
						out.println(",");
					}
					firstFunction = false;
					writeFunctionObject(out, highFunction, options.style);
					succeeded++;
				}
				catch (Exception ex) {
					Failure failure = new Failure();
					failure.entry = addressString(function.getEntryPoint());
					failure.name = function.getName();
					failure.stage = "decompile";
					failure.message = ex.getMessage();
					failures.add(failure);
					println("decompile failed: " + function.getName() + " " + failure.message);
				}
			}

			out.println();
			out.println("  ],");
			writeExternals(out);
			out.println(",");
			writeFailures(out, failures);
			out.println(",");
			writeModuleStats(out, attempted, succeeded, failures.size(), startNanos);
			out.println();
			out.println("}");
		}
		finally {
			decompiler.dispose();
		}

		long elapsedMs = (System.nanoTime() - startNanos) / 1000000;
		println("exported module to " + output.getAbsolutePath());
		println("attempted functions: " + attempted);
		println("succeeded functions: " + succeeded);
		println("failed functions: " + failures.size());
		println("external functions: " + externals.size());
		println("elapsed ms: " + elapsedMs);
	}

	private Options parseOptions(String[] args) {
		if (args.length < 1) {
			return null;
		}
		Options options = new Options();
		options.outputPath = args[0];
		for (int i = 1; i < args.length; ++i) {
			String arg = args[i];
			if (arg.equals("--all")) {
				options.limit = -1;
			}
			else if (arg.startsWith("--limit=")) {
				options.limit = Integer.parseInt(arg.substring("--limit=".length()));
			}
			else if (arg.startsWith("--timeout=")) {
				options.timeoutSec = Integer.parseInt(arg.substring("--timeout=".length()));
			}
			else if (arg.startsWith("--style=")) {
				options.style = arg.substring("--style=".length());
			}
			else if (arg.matches("[0-9]+")) {
				if (i == 1) {
					options.limit = Integer.parseInt(arg);
				}
				else {
					options.timeoutSec = Integer.parseInt(arg);
				}
			}
			else {
				options.style = arg;
			}
		}
		return options;
	}

	private List<Function> selectFunctions(int limit) {
		List<Function> selected = new ArrayList<>();
		FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
		while (functions.hasNext()) {
			Function function = functions.next();
			if (function.isExternal()) {
				rememberExternal(function, "function-manager");
				continue;
			}
			if (function.isThunk()) {
				continue;
			}
			if (limit >= 0 && selected.size() >= limit) {
				continue;
			}
			selected.add(function);
		}
		return selected;
	}

	private DecompInterface createDecompiler(String style) throws Exception {
		DecompInterface decompiler = new DecompInterface();
		DecompileOptions options = new DecompileOptions();
		decompiler.setOptions(options);
		decompiler.toggleCCode(false);
		decompiler.toggleSyntaxTree(true);
		decompiler.setSimplificationStyle(style);
		if (!decompiler.openProgram(currentProgram)) {
			throw new IllegalStateException("decompiler open failed: " + decompiler.getLastMessage());
		}
		return decompiler;
	}

	private HighFunction decompile(DecompInterface decompiler, Function function, int timeoutSec) throws Exception {
		DecompileResults results = decompiler.decompileFunction(function, timeoutSec, monitor);
		HighFunction highFunction = results.getHighFunction();
		if (highFunction == null) {
			throw new IllegalStateException("decompiler produced no HighFunction: " + results.getErrorMessage());
		}
		return highFunction;
	}

	private void writeProgram(PrintWriter out, String style) {
		out.println("  \"schema\": \"notdec.heritage-module.v0\",");
		out.println("  \"program\": {");
		out.println("    \"name\": " + json(currentProgram.getName()) + ",");
		out.println("    \"language\": " + json(currentProgram.getLanguage().toString()) + ",");
		out.println("    \"compilerSpec\": " + json(currentProgram.getCompilerSpec().toString()) + ",");
		out.println("    \"simplificationStyle\": " + json(style));
		out.print("  }");
	}

	private void writeFunctionObject(PrintWriter out, HighFunction highFunction, String style) {
		varnodes.clear();
		collectVarnodes(highFunction);
		registerVarnodeCount = countRegisterVarnodes();

		out.println("    {");
		out.println("      \"status\": \"ok\",");
		writeFunctionFields(out, highFunction);
		out.println(",");
		writeBlocks(out, highFunction);
		out.println(",");
		writeOps(out, highFunction);
		out.println(",");
		writeVarnodes(out);
		out.println(",");
		writeStats(out, highFunction);
		out.println();
		out.print("    }");
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
		if (symbol == null || symbol.getHighVariable() == null) {
			return;
		}
		remember(symbol.getHighVariable().getRepresentative());
	}

	private void remember(Varnode varnode) {
		if (varnode != null) {
			varnodes.putIfAbsent(vnodeId(varnode), varnode);
		}
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

	private void writeFunctionFields(PrintWriter out, HighFunction highFunction) {
		Function function = highFunction.getFunction();
		FunctionPrototype prototype = highFunction.getFunctionPrototype();
		out.println("      \"name\": " + json(function.getName()) + ",");
		out.println("      \"entry\": " + json(addressString(function.getEntryPoint())) + ",");
		out.println("      \"callingConvention\": " + json(function.getCallingConventionName()) + ",");
		out.println("      \"returnType\": " + json(typeString(prototype != null ? prototype.getReturnType() : null)) + ",");
		out.println("      \"params\": [");
		if (prototype != null) {
			for (int i = 0; i < prototype.getNumParams(); ++i) {
				if (i != 0) {
					out.println(",");
				}
				writeParam(out, prototype.getParam(i), i);
			}
		}
		out.println();
		out.println("      ]");
	}

	private void writeParam(PrintWriter out, HighSymbol symbol, int index) {
		out.print("        {");
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
		out.println("      \"blocks\": [");
		boolean first = true;
		for (PcodeBlockBasic block : highFunction.getBasicBlocks()) {
			if (!first) {
				out.println(",");
			}
			first = false;
			out.println("        {");
			out.println("          \"id\": " + json(blockId(block)) + ",");
			out.println("          \"index\": " + block.getIndex() + ",");
			out.println("          \"start\": " + json(addressString(block.getStart())) + ",");
			writeBlockRefs(out, "in", block, true);
			out.println(",");
			writeBlockRefs(out, "out", block, false);
			out.println(",");
			out.println("          \"ops\": [");
			Iterator<PcodeOp> ops = block.getIterator();
			boolean firstOp = true;
			while (ops.hasNext()) {
				PcodeOp op = ops.next();
				if (!firstOp) {
					out.println(",");
				}
				firstOp = false;
				out.print("            " + json(opId(op)));
			}
			out.println();
			out.println("          ]");
			out.print("        }");
		}
		out.println();
		out.print("      ]");
	}

	private void writeBlockRefs(PrintWriter out, String name, PcodeBlockBasic block, boolean incoming) {
		out.print("          \"" + name + "\": [");
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
		out.println("      \"ops\": [");
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
		out.print("      ]");
	}

	private void writeOp(PrintWriter out, PcodeOp op) {
		SequenceNumber seq = op.getSeqnum();
		out.println("        {");
		out.println("          \"id\": " + json(opId(op)) + ",");
		out.println("          \"parent\": " + json(blockId(op.getParent())) + ",");
		out.println("          \"seqTarget\": " + json(seq != null ? addressString(seq.getTarget()) : null) + ",");
		out.println("          \"seqTime\": " + (seq != null ? seq.getTime() : -1) + ",");
		out.println("          \"opcode\": " + op.getOpcode() + ",");
		out.println("          \"mnemonic\": " + json(op.getMnemonic()) + ",");
		out.println("          \"text\": " + json(op.toString()) + ",");
		out.println("          \"callTarget\": " + json(directCallTarget(op)) + ",");
		out.println("          \"callTargetName\": " + json(directCallTargetName(op)) + ",");
		out.println("          \"output\": " + json(vnodeId(op.getOutput())) + ",");
		out.println("          \"inputs\": [");
		for (int i = 0; i < op.getNumInputs(); ++i) {
			if (i != 0) {
				out.println(",");
			}
			out.print("            " + json(vnodeId(op.getInput(i))));
		}
		out.println();
		out.println("          ]");
		out.print("        }");
	}

	private void writeVarnodes(PrintWriter out) {
		out.println("      \"varnodes\": [");
		boolean first = true;
		for (Varnode varnode : varnodes.values()) {
			if (!first) {
				out.println(",");
			}
			first = false;
			writeVarnode(out, varnode);
		}
		out.println();
		out.print("      ]");
	}

	private void writeVarnode(PrintWriter out, Varnode varnode) {
		HighVariable highVariable = varnode.getHigh();
		out.println("        {");
		out.println("          \"id\": " + json(vnodeId(varnode)) + ",");
		out.println("          \"space\": " + json(varnode.getAddress().getAddressSpace().getName()) + ",");
		out.println("          \"offset\": " + json(Long.toUnsignedString(varnode.getOffset())) + ",");
		out.println("          \"size\": " + varnode.getSize() + ",");
		out.println("          \"address\": " + json(addressString(varnode.getAddress())) + ",");
		out.println("          \"pcAddress\": " + json(addressString(varnode.getPCAddress())) + ",");
		out.println("          \"registerName\": " + json(registerName(varnode)) + ",");
		out.println("          \"isInput\": " + varnode.isInput() + ",");
		out.println("          \"isRegister\": " + varnode.isRegister() + ",");
		out.println("          \"isUnique\": " + varnode.isUnique() + ",");
		out.println("          \"isConstant\": " + varnode.isConstant() + ",");
		out.println("          \"isAddressTied\": " + varnode.isAddrTied() + ",");
		out.println("          \"highVariable\": " + json(highVariable != null ? highVariable.getName() : null) + ",");
		out.println("          \"highType\": " + json(highVariable != null ? typeString(highVariable.getDataType()) : null) + ",");
		out.println("          \"def\": " + json(varnode.getDef() != null ? opId(varnode.getDef()) : null));
		out.print("        }");
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

		out.println("      \"stats\": {");
		out.println("        \"blockCount\": " + highFunction.getBasicBlocks().size() + ",");
		out.println("        \"opCount\": " + opCount + ",");
		out.println("        \"varnodeCount\": " + varnodes.size() + ",");
		out.println("        \"registerVarnodeCount\": " + registerVarnodeCount + ",");
		out.println("        \"multiequalCount\": " + multiequalCount);
		out.print("      }");
	}

	private void writeExternals(PrintWriter out) {
		out.println("  \"externals\": [");
		boolean first = true;
		for (Function external : externals.values()) {
			if (!first) {
				out.println(",");
			}
			first = false;
			out.print("    {");
			out.print("\"name\": " + json(external.getName()));
			out.print(", \"address\": " + json(addressString(external.getEntryPoint())));
			out.print(", \"returnType\": " + json(typeString(external.getReturnType())));
			out.print(", \"params\": []");
			out.print(", \"source\": \"external\"");
			out.print("}");
		}
		out.println();
		out.print("  ]");
	}

	private void writeFailures(PrintWriter out, Set<Failure> failures) {
		out.println("  \"failures\": [");
		boolean first = true;
		for (Failure failure : failures) {
			if (!first) {
				out.println(",");
			}
			first = false;
			out.print("    {");
			out.print("\"entry\": " + json(failure.entry));
			out.print(", \"name\": " + json(failure.name));
			out.print(", \"stage\": " + json(failure.stage));
			out.print(", \"message\": " + json(failure.message));
			out.print("}");
		}
		out.println();
		out.print("  ]");
	}

	private void writeModuleStats(PrintWriter out, int attempted, int succeeded, int failed, long startNanos) {
		long elapsedMs = (System.nanoTime() - startNanos) / 1000000;
		out.println("  \"stats\": {");
		out.println("    \"attemptedFunctionCount\": " + attempted + ",");
		out.println("    \"functionCount\": " + succeeded + ",");
		out.println("    \"failureCount\": " + failed + ",");
		out.println("    \"externalCount\": " + externals.size() + ",");
		out.println("    \"elapsedMs\": " + elapsedMs);
		out.print("  }");
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
		if (function == null) {
			return null;
		}
		if (function.isExternal() || !seenEntries.contains(addressString(function.getEntryPoint()))) {
			rememberExternal(function, "call-target");
		}
		return function.getName();
	}

	private void rememberExternal(Function function, String source) {
		if (function == null) {
			return;
		}
		externals.putIfAbsent(function.getName(), function);
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
