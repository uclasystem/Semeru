public class Test{
public static void main(String[] args) {
        int _1m = 1024 * 1024;
        byte[] data = new byte[_1m];
        data = null;
        System.gc();
    }

}

