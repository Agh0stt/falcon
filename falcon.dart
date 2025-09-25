import 'dart:io';

// falcon (falconc FalconCompiler) - copyright -: 2025-Present Abhigyan Ghosh. All rights reserved.

// Global variable to prevent circular includes across all files
final Set<String> includedFiles = {}; 

/// Recursively processes a Falcon source file and its nested 'incl' directives.
List<String> processInclusions(String filePath) {
  if (includedFiles.contains(filePath)) {
    print("Warning: Skipping duplicate inclusion of $filePath");
    return [];
  }
  includedFiles.add(filePath);

  List<String> processedLines = [];
  try {
    final source = File(filePath).readAsLinesSync();
    for (var line in source) {
      final trimmedLine = line.trim();
      if (trimmedLine.startsWith('incl =')) {
        final filePathMatch = RegExp(r'incl\s*=\s*"([^"]+)"').firstMatch(trimmedLine);
        if (filePathMatch != null) {
          final includePath = filePathMatch.group(1)!;
          // Recursively call to process the included file
          processedLines.addAll(processInclusions(includePath));
        } else {
          print("Error: Invalid 'incl' syntax in '$filePath'. Use: incl = \"filename.fl\"");
          exit(1);
        }
      } else {
        processedLines.add(line);
      }
    }
  } catch (e) {
    print("Error: Could not read file '$filePath': $e");
    exit(1);
  }
  return processedLines;
}

void main(List<String> args) {
  if (args.length < 3) {
    print("Usage: dart falcon.dart <source.fl> <output.dart>");
    exit(1);
  }
 
  final sourceFile = args[0];
  final outputDart = args[1];
  final outputBinary = args[2];
  
  // Pass 1: Pre-processing. This handles all 'incl' directives.
  final List<String> allSourceLines = processInclusions(sourceFile);
  
  // Define inRawBlock here, before the main compilation loop
  bool inRawBlock = false; 
  
  // Begin the Dart code generation
  final dartCode = StringBuffer('''
import 'dart:io';
import 'dart:math';

// Falcon Standard Library (auto added)
String readFile(String path) {
  return File(path).readAsStringSync();
}

void writeFile(String path, String contents) {
  File(path).writeAsStringSync(contents);
}

''');
  
  int fallthroughLabelCounter = 0;
  
  // Pass 2: The main compilation loop. It now iterates over the pre-processed lines.
  for (var line in allSourceLines) {
    // Check for raw block first and handle exclusively
    if (inRawBlock) {
      if (line.trim() == '}') {
        inRawBlock = false;
        continue;
      } else {
        dartCode.writeln('$line'); 
      }
      continue;
    } else if (line.trim().startsWith('raw {')) {
      inRawBlock = true;
      continue;
    }

    final trimmedLine = line.trim();

    // ===== Include Files
    // This logic is for 'include = ""' which blindly pastes content.
    // 'incl' for pre-processing is already handled above.
    if (trimmedLine.startsWith('include =')) {
      final filePathMatch = RegExp(r'include\s*=\s*"([^"]+)"').firstMatch(trimmedLine);
      if (filePathMatch != null) {
        final filePath = filePathMatch.group(1)!;

        // Check for circular includes
        if (includedFiles.contains(filePath)) {
          print("Warning: Skipping duplicate include of $filePath");
          continue;
        }

        includedFiles.add(filePath);

        try {
          final includedContent = File(filePath).readAsLinesSync();
          for (var includedLine in includedContent) {
            dartCode.writeln(includedLine);
          }
        } catch (e) {
          print("Error: Could not read included file '$filePath': $e");
          exit(1);
        }
      } else {
        print("Error: Invalid include syntax. Use: include = \"filename.header\"");
        exit(1);
      }
      continue; // Skip the rest of the loop for this line.
    }
    
    // ===== Import Files (blind paste)
    else if (trimmedLine.startsWith('import ')) {
      final filePathMatch = RegExp(r'import\s+"([^"]+)"').firstMatch(trimmedLine);
      if (filePathMatch != null) {
        final filePath = filePathMatch.group(1)!;
        try {
          final importedContent = File(filePath).readAsLinesSync();
          for (var importedLine in importedContent) {
            dartCode.writeln(importedLine);
          }
        } catch (e) {
          print("Error: Could not read imported file '$filePath': $e");
          exit(1);
        }
      } else {
        print("Error: Invalid import syntax. Use: import \"filename.fl\"");
        exit(1);
      }
      continue; // Skip processing this line further
    }
    
    // ===== print with exact Dart syntax
    if (trimmedLine.startsWith('print(')) {
      // Take everything inside the parentheses as-is
      var inside = trimmedLine.substring(6, trimmedLine.length - 1).trim();
      dartCode.writeln('  print($inside);');
    }
    // ===== File IO: readFile("path")
    else if (trimmedLine.startsWith('readFile(')) {
      final inside = trimmedLine.substring(9, trimmedLine.length - 1);
      dartCode.writeln('  File($inside).readAsStringSync();');
    }
    else if (trimmedLine.startsWith("final ")) {
      dartCode.writeln(trimmedLine + ";");
    }
    // ===== File IO: writeFile("path", "content")
    else if (trimmedLine.startsWith('writeFile(')) {
      final args = trimmedLine.substring(10, trimmedLine.length - 1).split(',');
      if (args.length >= 2) {
        final path = args[0].trim();
        final content = args.sublist(1).join(',').trim();
        dartCode.writeln('  File($path).writeAsStringSync($content);');
      }
    }
    
    // ===== File IO: fileExists("path")
    else if (trimmedLine.startsWith('fileExists(')) {
      final inside = trimmedLine.substring(11, trimmedLine.length - 1);
      dartCode.writeln('  File($inside).existsSync();');
    }
    // ===== Standalone input() with variable assignment
    else if (trimmedLine.startsWith('input(')) {
      final inside = trimmedLine.substring(6, trimmedLine.length - 1);
      final args = inside.split(',').map((s) => s.trim()).toList();
      if (args.length >= 2) {
        final typeAndName = args[0].split(RegExp(r'\s+'));
        if (typeAndName.length != 2) {
          throw Exception("Invalid input() syntax. Use: input(type name, \"prompt\")");
        }
        final type = typeAndName[0];
        final name = typeAndName[1];
        final prompt = args[1];
        String dartInput;
        switch (type) {
          case 'int':
            dartInput = 'int.parse(stdin.readLineSync()!)';
            break;
          case 'double':
            dartInput = 'double.parse(stdin.readLineSync()!)';
            break;
          case 'bool':
            dartInput = 'stdin.readLineSync()!.toLowerCase() == "true"';
            break;
          case 'str':
          case 'String':
          default:
            dartInput = 'stdin.readLineSync()!';
        }
        dartCode.writeln('  stdout.write($prompt);');
        dartCode.writeln('  var $name = $dartInput;');
      }
    }

    // ===== File IO: deleteFile("path")
    else if (trimmedLine.startsWith('deleteFile(')) {
      final inside = trimmedLine.substring(11, trimmedLine.length - 1);
      dartCode.writeln('  File($inside).deleteSync();');
    }

    // Assignment with function call
    else if (trimmedLine.startsWith('let ') || trimmedLine.startsWith('const ')) {
      final parts = trimmedLine.split('=');
      final name = parts[0].replaceAll(RegExp(r'^(let|const)\s+'), '').trim();
      final value = parts.sublist(1).join('=').trim();
      if (trimmedLine.startsWith('let ')) {
        dartCode.writeln('  var $name = $value;');
      } else {
        dartCode.writeln('  const $name = $value;');
      }
    }
    else if (trimmedLine.startsWith('msleep(')) {
      final inside = trimmedLine.substring(7, trimmedLine.length - 1);
      dartCode.writeln('  sleep(Duration(milliseconds: $inside));');
    }
    else if (trimmedLine.startsWith('sleep(')) {
      final inside = trimmedLine.substring(6, trimmedLine.length - 1);
      dartCode.writeln('  sleep(Duration(seconds: $inside));');
    }
    
    // do while loop
    else if (trimmedLine.startsWith('do {')) {
      dartCode.writeln('  do {');
    }
    else if (trimmedLine.startsWith('} while')) {
      final condition = trimmedLine.substring(2).trim();
      dartCode.writeln('  } $condition;');
    } else if (trimmedLine.startsWith('array ')) {
      final parts = trimmedLine.split('=');
      final name = parts[0].replaceAll('array', '').trim();
      final values = parts.length > 1 ? parts[1].trim() : '[]';
      dartCode.writeln('  List<dynamic> $name = $values;');
    } else if (trimmedLine.startsWith('enum ')) {
      final enumMatch = RegExp(r'enum\s+(\w+)\s*\{(.*?)\}').firstMatch(trimmedLine);
      if (enumMatch != null) {
        final enumName = enumMatch.group(1)!;
        final enumBody = enumMatch.group(2)!;
        final values = enumBody.split(',')
            .map((v) => v.trim())
            .where((v) => v.isNotEmpty)
            .map((v) {
              if (v.contains('=')) {
                final parts = v.split('=');
                final key = parts[0].trim();
                var value = parts[1].trim();
                if (!RegExp(r'^-?\d+$').hasMatch(value)) {
                  if (!value.startsWith('"')) {
                    value = '"$value"';
                  }
                }
                return 'static const $key = $value;';
              } else {
                return 'static const $v = 0;';
              }
            })
            .join('\n  ');
        dartCode.writeln('class $enumName {\n  $values\n}');
      }
    }

    // ===== Functions (C-like syntax, strict typing, no semicolon in Falcon)
    else if (RegExp(r'^(void|int|bool|str|double)\s+\w+\s*\(').hasMatch(trimmedLine)) {
      final funcMatch = RegExp(r'^(void|int|bool|str|double)\s+(\w+)\((.*?)\)')
          .firstMatch(trimmedLine);
      if (funcMatch != null) {
        var returnTypeKeyword = funcMatch.group(1)!;
        final funcName = funcMatch.group(2)!;
        final params = funcMatch.group(3)!;
        switch (returnTypeKeyword) {
          case 'int': returnTypeKeyword = 'int'; break;
          case 'bool': returnTypeKeyword = 'bool'; break;
          case 'str': returnTypeKeyword = 'String'; break;
          case 'double': returnTypeKeyword = 'double'; break;
          case 'void': default: returnTypeKeyword = 'void';
        }
        final typedParams = params.trim().isEmpty
            ? ''
            : params.split(',').map((p) {
                final parts = p.trim().split(RegExp(r'\s+'));
                if (parts.length != 2) {
                  throw Exception("Invalid parameter in function $funcName. Use: type name");
                }
                var type = parts[0];
                if (type == 'str') type = 'String';
                return '$type ${parts[1]}';
              }).join(', ');
        dartCode.writeln('$returnTypeKeyword $funcName($typedParams) {');
      }
    }
    // ===== Return statements
    else if (trimmedLine.startsWith('return')) {
      var returnContent = trimmedLine.substring(6).trim();
      if (returnContent.isEmpty) {
        dartCode.writeln('  return;');
      } else {
        dartCode.writeln('  return $returnContent;');
      }
    }
    
    else if (trimmedLine.startsWith('switch ')) {
      final expr = trimmedLine.substring(7, trimmedLine.length - 1).trim();
      dartCode.writeln('switch ($expr) {');
    }
    
        else if (trimmedLine.startsWith('switch ')) {
      final expr = trimmedLine.substring(7, trimmedLine.length - 1).trim();
      dartCode.writeln('switch ($expr) {');
    }
    else if (trimmedLine.startsWith('case ')) {
      final value = trimmedLine.substring(5).trim();
      // Remove trailing colon if present to avoid double colons
      final cleanValue = value.endsWith(':') ? value.substring(0, value.length - 1) : value;
      final label = 'label${fallthroughLabelCounter++}';
      dartCode.writeln('$label:');
      dartCode.writeln('  case $cleanValue:');
    }
    else if (trimmedLine == 'fallthrough') {
      final nextLabel = 'label${fallthroughLabelCounter}';
      dartCode.writeln('    continue $nextLabel;');
    }
    else if (trimmedLine == 'break') {
      dartCode.writeln('    break;');
    }
    else if (trimmedLine.startsWith('default')) {
      dartCode.writeln('  default:');
    }

    else if (trimmedLine.startsWith('if ') || trimmedLine.startsWith('else if') || trimmedLine.startsWith('else')) {
      dartCode.writeln('  $trimmedLine');
    }
    // ===== Classes
    else if (trimmedLine.startsWith("class ")) {
      final classMatch = RegExp(r'class\s+(\w+)\s*\{').firstMatch(trimmedLine);
      if (classMatch != null) {
        final className = classMatch.group(1)!;
        dartCode.writeln("class $className {");
      }
    }
    // ===== Closing brace inside class
    else if (trimmedLine == "}") {
      dartCode.writeln("}");
    }
    // ===== Functions inside class (must be static)
    else if (RegExp(r'^(static\s+)?(void|int|bool|str|double)\s+\w+\s*\(').hasMatch(trimmedLine)) {
      final funcMatch = RegExp(r'^(static\s+)?(void|int|bool|str|double)\s+(\w+)\((.*?)\)')
          .firstMatch(trimmedLine);
      if (funcMatch != null) {
        final staticKeyword = funcMatch.group(1) ?? "";
        var returnType = funcMatch.group(2)!;
        final funcName = funcMatch.group(3)!;
        final params = funcMatch.group(4)!;
        switch (returnType) {
          case "int": returnType = "int"; break;
          case "bool": returnType = "bool"; break;
          case "str": returnType = "String"; break;
          case "double": returnType = "double"; break;
          case "void": default: returnType = "void";
        }
        final typedParams = params.trim().isEmpty
            ? ""
            : params.split(",").map((p) {
                final parts = p.trim().split(RegExp(r'\s+'));
                if (parts.length != 2) {
                  throw Exception("Invalid parameter in function $funcName. Use: type name");
                }
                var type = parts[0];
                if (type == "str") type = "String";
                return "$type ${parts[1]}";
              }).join(", ");
        if (!staticKeyword.trim().startsWith("static")) {
          throw Exception("Error: Functions inside classes must be declared static.");
        }
        dartCode.writeln("  $staticKeyword$returnType $funcName($typedParams) {");
      }
      // Variable declarations (int, double, bool, str/String)
    else if (RegExp(r'^(int|double|bool|str|String)\s+\w+\s*=').hasMatch(trimmedLine)) {
      var processedLine = trimmedLine;
      // Convert 'str' to 'String' for Dart compatibility
      if (processedLine.startsWith('str ')) {
        processedLine = processedLine.replaceFirst('str ', 'String ');
      }
      dartCode.writeln('  $processedLine;');
    }
    }
    // If line ends with ')' and is not a control statement
    else if (trimmedLine.endsWith(')') && !trimmedLine.endsWith(';') &&
             !trimmedLine.startsWith('if') && !trimmedLine.startsWith('while') &&
             !trimmedLine.startsWith('for')) {
      dartCode.writeln('  $trimmedLine;'); // keep exactly as-is
    }
    else {
      dartCode.writeln('  $trimmedLine');
    }
  }
 
  // Finalize and write the output
  File(outputDart).writeAsStringSync(dartCode.toString());
  final result = Process.runSync('dart', ['compile', 'exe', outputDart, '-o', outputBinary]);
  if (result.exitCode == 0) {
    print("falconc (falconCompiler) successfully Compiled $sourceFile to $outputBinary");
  } else {
    print("Compilation failed: ${result.stderr}");
  }
}

