import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** Exercises the actual extracted production filter; no Android stand-ins. */
public final class ResearchEnvironmentTest {
    private static int checks;
    private static void check(boolean value) {
        checks++;
        if (!value) throw new AssertionError("check " + checks);
    }
    public static void main(String[] args) {
        String[] research = {
            "AUDIO_WAV", "LOG_SKIPS", "SAVE_MSL", "SAVE_BLOB", "SYNC_SHADERS", "HOST_INPUT", "INPUT",
            "NO_USER_INPUT", "NCE_FAULT_TRACE", "MATERIAL_PROBE", "GPU_UNIFORM_FORWARD", "VK_D24",
            "VK_EQUAL_TOL", "VK_COPY_LAZY", "VK_COPY_OPT", "SYNC_FLUSH", "SYNC_LAG", "GCOPY_PROBE",
            "VK_EXACT_H2F", "VK_PRESENT", "LOG_INTERVAL"
        };
        String keep = "# Keep comments and unknown owner settings\n"
            + "LARECOMP_CPU_BACKEND=continuous-nce\nLARECOMP_SCALE=0.6666666666666666\nLC_ALL=en_US\n"
            + "LARECOMP_PACKAGE_URI=content://owner/game.nsp\nLARECOMP_PACKAGE_KEYS=/private/prod.keys\n"
            + "LARECOMP_PACKAGE_PATH=/owner/game.nsp\nLARECOMP_SETTINGS=/private/settings.ini\n"
            + "LARECOMP_VK_ACQUIRE_FENCE=0\nLARECOMP_TRACE=keep\n"
            + "LARECOMP_VK_D24_EXTRA=keep\nOTHER=LARECOMP_VK_D24=1\n"
            + "LITERAL=first\\nLARECOMP_VK_D24=1\\r\\nlast\n";
        for (String ending : new String[]{"\n", "\r\n"}) {
            StringBuilder input = new StringBuilder(keep.replace("\n", ending));
            for (String flag : research)
                for (String value : new String[]{"", "0", "1", "diagnostic-path"})
                    input.append("  LARECOMP_").append(flag).append(" =").append(value).append(ending);
            String filtered = SharedFilesFilter.withoutResearchEnvironment(input.toString());
            check(filtered.equals(keep));
            check(SharedFilesFilter.withoutResearchEnvironment(filtered).equals(filtered));
            for (String flag : research)
                check(!filtered.contains("\nLARECOMP_" + flag + "="));
            // Every individual flag is removed even as the first/only line.
            for (String flag : research)
                check(SharedFilesFilter.withoutResearchEnvironment("LARECOMP_" + flag + "=0" + ending).isEmpty());
        }
        check(SharedFilesFilter.withoutResearchEnvironment("").isEmpty());
        check(SharedFilesFilter.withoutResearchEnvironment("\n\r\n").isEmpty());
        check(SharedFilesFilter.withoutResearchEnvironment("OWNER=value").equals("OWNER=value\n"));
        check(SharedFilesFilter.withoutResearchEnvironment("# LARECOMP_VK_D24=1\n").equals("# LARECOMP_VK_D24=1\n"));
        System.out.println("{\"state\":\"passed\",\"checks\":" + checks + "}");
    }
}
