public final class Repro {
    private static final String LIB_DIR = "/data/local/tmp/qbdi-art-stack";

    private static native int run(String mode);

    public static int ping() {
        return 42;
    }

    public static void main(String[] args) {
        String mode = args.length == 0 ? "call-findclass" : args[0];
        System.load(LIB_DIR + "/libqbdirepro.so");
        System.out.println("[java] mode=" + mode);
        int result = run(mode);
        System.out.println("[java] native result=" + result);
    }
}
