package psx.exporter;

import java.io.InputStream;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.HexFormat;

final class Identity {
    private Identity() {}
    static String hash(byte[] bytes) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }
    static String hash(Path path) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] block = new byte[65536];
        try (InputStream in = Files.newInputStream(path)) {
            for (int n; (n = in.read(block)) >= 0;) if (n != 0) digest.update(block, 0, n);
        }
        return HexFormat.of().formatHex(digest.digest());
    }
    static String json(String value) {
        StringBuilder s = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '\\' || c == '"') s.append('\\').append(c);
            else if (c < 32) s.append(String.format("\\u%04x", (int)c));
            else s.append(c);
        }
        return s.append('"').toString();
    }
}
