import 'dart:io';
// falcon (falconc FalconCompiler) - copyrigjt -: 2025-Present Abhigyan Ghosh. All rights reserved.
void main(List<String> args) {
  if (args.length < 3) {
    print("Usage: dart falcon.dart <source.fl> <output.dart>");
    exit(1);
  }
 final includedFiles = <String>{}; // Keeps track of already included files
  final sourceFile = args[0];
  final outputDart = args[1];
  final outputBinary = args[2];

  final source = File(sourceFile).readAsLinesSync();

  final dartCode = StringBuffer('''
import 'dart:io';

// Falcon Standard Library (auto added)
String readFile(String path) {
  return File(path).readAsStringSync();
}

void writeFile(String path, String contents) {
  File(path).writeAsStringSync(contents);
}

void main() {
''');
int fallthroughLabelCounter = 0;
  for (var line in source) {
    line = line.trim();

    // print with comma concat and interpolation
 if (line.startsWith('print(')) {
  var inside = line.substring(6, line.length - 1).trim();

  // Replace commas with + for concatenation
  inside = inside.replaceAll(',', '+');

  // Handle string interpolation: replace $var with ${var}
  inside = inside.replaceAllMapped(RegExp(r'\$([a-zA-Z_]\w*)'), (match) {
    return '\${${match.group(1)}}';
  });

  dartCode.writeln('  print($inside);');
}
// ===== File IO: readFile("path")
else if (line.startsWith('readFile(')) {
  final inside = line.substring(9, line.length - 1);
  dartCode.writeln('  File($inside).readAsStringSync();');
}

// ===== File IO: writeFile("path", "content")
else if (line.startsWith('writeFile(')) {
  final args = line.substring(10, line.length - 1).split(',');
  if (args.length >= 2) {
    final path = args[0].trim();
    final content = args.sublist(1).join(',').trim();
    dartCode.writeln('  File($path).writeAsStringSync($content);');
  }
}

// ===== File IO: fileExists("path")
else if (line.startsWith('fileExists(')) {
  final inside = line.substring(11, line.length - 1);
  dartCode.writeln('  File($inside).existsSync();');
}

// ===== File IO: deleteFile("path")
else if (line.startsWith('deleteFile(')) {
  final inside = line.substring(11, line.length - 1);
  dartCode.writeln('  File($inside).deleteSync();');
}

    // let var = value
    else if (line.startsWith('let ')) {
      final parts = line.split(RegExp(r'\s+'));
      final name = parts[1];
      final value = parts[3];
      dartCode.writeln('  var $name = $value;');
    } 
    else if (line.startsWith('msleep(')) {
  final inside = line.substring(7, line.length - 1); // value inside ()
  dartCode.writeln('  sleep(Duration(milliseconds: $inside));');
}
     else if (line.startsWith('sleep(')) {
  final inside = line.substring(6, line.length - 1); // get number inside ()
  dartCode.writeln('  sleep(Duration(seconds: $inside));');
}
    // const var = value
    else if (line.startsWith('const ')) {
      final parts = line.split(RegExp(r'\s+'));
      final name = parts[1];
      final value = parts[3];
      dartCode.writeln('  const $name = $value;');
    } // while loop
else if (line.startsWith('while ')) {
  dartCode.writeln('  $line {');
}

// do while loop
else if (line.startsWith('do {')) {
  dartCode.writeln('  do {');
}
else if (line.startsWith('} while')) {
  final condition = line.substring(2).trim(); // grabs "while (x < 10)"
  dartCode.writeln('  } $condition;');
} else if (line.startsWith('array ')) {
  final parts = line.split('=');
  final name = parts[0].replaceAll('array', '').trim();
  final values = parts.length > 1 ? parts[1].trim() : '[]';
  dartCode.writeln('  List<dynamic> $name = $values;');
} else if (line.startsWith('enum ')) {
  final enumMatch = RegExp(r'enum\s+(\w+)\s*\{(.*?)\}').firstMatch(line);
  if (enumMatch != null) {
    final enumName = enumMatch.group(1)!;
    final enumBody = enumMatch.group(2)!;

    // Parse members
    final values = enumBody.split(',')
        .map((v) => v.trim())
        .where((v) => v.isNotEmpty)
        .map((v) {
          if (v.contains('=')) {
            final parts = v.split('=');
            final key = parts[0].trim();
            var value = parts[1].trim();
            
            // Add quotes for string literals if missing
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
else if (RegExp(r'^(void|int|bool|str|double)\s+\w+\s*\(').hasMatch(line)) {
  final funcMatch = RegExp(r'^(void|int|bool|str|double)\s+(\w+)\((.*?)\)')
      .firstMatch(line);

  if (funcMatch != null) {
    var returnTypeKeyword = funcMatch.group(1)!;
    final funcName = funcMatch.group(2)!;
    final params = funcMatch.group(3)!;

    // Map Falcon types to Dart types
    switch (returnTypeKeyword) {
      case 'int':
        returnTypeKeyword = 'int';
        break;
      case 'bool':
        returnTypeKeyword = 'bool';
        break;
      case 'str':
        returnTypeKeyword = 'String';
        break;
      case 'double':
        returnTypeKeyword = 'double';
        break;
      case 'void':
      default:
        returnTypeKeyword = 'void';
    }

    // Force typed parameters like C
    final typedParams = params.trim().isEmpty
        ? ''
        : params.split(',')
            .map((p) {
              final parts = p.trim().split(RegExp(r'\s+'));
              if (parts.length != 2) {
                throw Exception("Invalid parameter in function $funcName. Use: type name");
              }
              var type = parts[0];
              if (type == 'str') type = 'String';
              return '$type ${parts[1]}';
            })
            .join(', ');

    dartCode.writeln('$returnTypeKeyword $funcName($typedParams) {');
  }
}
    // ===== Return statements
else if (line.trim().startsWith('return')) {
  var returnContent = line.trim().substring(6).trim();

  // If nothing after return → just return;
  if (returnContent.isEmpty) {
    dartCode.writeln('  return;');
  } else {
    dartCode.writeln('  return $returnContent;');
  }
}
    
else if (line.startsWith('switch ')) {
  final expr = line.substring(7, line.length - 1).trim();
  dartCode.writeln('switch ($expr) {');
}

else if (line.startsWith('case ')) {
  final value = line.substring(5).trim();
  final label = 'label${fallthroughLabelCounter++}';
  dartCode.writeln('$label:');
  dartCode.writeln('  case $value:');
}

else if (line.trim() == 'fallthrough') {
  final nextLabel = 'label${fallthroughLabelCounter}';
  dartCode.writeln('    continue $nextLabel;');
}

else if (line.trim() == 'break') {
  dartCode.writeln('    break;');
}

else if (line.startsWith('default')) {
  dartCode.writeln('  default:');
}

else if (line.trim() == '}') {
  dartCode.writeln('}');
}


   else if (line.startsWith('if ') || line.startsWith('else if') || line.startsWith('else')) {
  dartCode.writeln('  $line');
}
else if (line.startsWith('}')) {
  dartCode.writeln('  }');
}
else if (line.endsWith(')') && !line.endsWith(';')) {
  dartCode.writeln('  $line;'); // auto add semicolon
}
else {
  dartCode.writeln('  $line');
}
  }

  dartCode.writeln('}');

  File(outputDart).writeAsStringSync(dartCode.toString());


  // Compile Dart to machine code
  final result = Process.runSync('dart', ['compile', 'exe', outputDart, '-o', outputBinary]);
  if (result.exitCode == 0) {
    print("falconc (falconCompiler) succesfully  Compiled $sourceFile to  $outputBinary");
  } else {
    print("Compilation failed: ${result.stderr}");
  }
}
